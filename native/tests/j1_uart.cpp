#include "../src/core.h"
#include "../src/board_config.h"
#include <winrt/base.h>
#include <iostream>
static Bytes cfg(int m,unsigned hz,int a=0,int b=0,int c=0){Bytes p{(uint8_t)m};put32(p,hz);p.push_back(a);p.push_back(b);p.push_back(c);return p;}
int wmain(int argc,wchar_t**argv){HANDLE serial=INVALID_HANDLE_VALUE;try{
 if(argc!=2)throw std::runtime_error("Specify the verified USB TTL COM port");
 winrt::init_apartment(winrt::apartment_type::multi_threaded);WSADATA ws{};WSAStartup(MAKEWORD(2,2),&ws);
 serial=CreateFileW((std::wstring(L"\\\\.\\")+argv[1]).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(serial==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open USB TTL serial port");
 DCB d{sizeof(d)};if(!GetCommState(serial,&d))throw std::runtime_error("GetCommState failed");d.BaudRate=115200;d.ByteSize=8;d.Parity=NOPARITY;d.StopBits=ONESTOPBIT;d.fBinary=TRUE;d.fParity=FALSE;d.fOutxCtsFlow=FALSE;d.fOutxDsrFlow=FALSE;d.fDtrControl=DTR_CONTROL_DISABLE;d.fRtsControl=RTS_CONTROL_DISABLE;d.fOutX=FALSE;d.fInX=FALSE;d.fAbortOnError=FALSE;if(!SetCommState(serial,&d))throw std::runtime_error("SetCommState failed");COMMTIMEOUTS t{MAXDWORD,0,50,0,1000};if(!SetCommTimeouts(serial,&t))throw std::runtime_error("SetCommTimeouts failed");PurgeComm(serial,PURGE_RXCLEAR|PURGE_TXCLEAR);
 auto connect=[](){return connect_ble(0xE83DC1D25042ULL,BOARD_TOKEN);};auto link=connect();auto b=link->request(3);std::string status(b.begin(),b.end());std::cout<<status<<std::endl;int mv=json_int(status,"vref_mv");if(mv<2700||mv>3600)throw std::runtime_error("Connect adapter 3.3V to J1-3 VREF first");
 link->request(17,cfg(1,115200,0,8,1));
 for(int round=0;round<3;round++){
  Bytes tx(96);for(size_t i=0;i<tx.size();i++)tx[i]=(uint8_t)(i*17+round*43);DWORD written=0;if(!WriteFile(serial,tx.data(),(DWORD)tx.size(),&written,nullptr)||written!=tx.size())throw std::runtime_error("USB TTL write failed");
  Bytes rx;auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(rx.size()<tx.size()&&std::chrono::steady_clock::now()<until){auto q=link->request(19,Bytes{1});rx.insert(rx.end(),q.begin(),q.end());std::this_thread::sleep_for(std::chrono::milliseconds(20));}if(rx!=tx)throw std::runtime_error("USB TTL TX -> J1 RX1 -> BLE byte mismatch");
  tx.resize(240);for(size_t i=0;i<tx.size();i++)tx[i]=(uint8_t)(i*19+round*41);([&]{Bytes packet{1};packet.insert(packet.end(),tx.begin(),tx.end());return link->request(18,packet);}());rx.clear();until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(rx.size()<tx.size()&&std::chrono::steady_clock::now()<until){uint8_t buf[256];DWORD got=0;if(!ReadFile(serial,buf,sizeof(buf),&got,nullptr))throw std::runtime_error("USB TTL read failed");rx.insert(rx.end(),buf,buf+got);}if(rx!=tx)throw std::runtime_error("BLE -> J1 TX1 -> USB TTL RX byte mismatch");
  std::cout<<"PASS UART1 physical bidirectional round "<<round+1<<" (96 RX + 240 TX bytes)"<<std::endl;
 }
 link->request(17,cfg(2,115200,2,8,2));link->request(18,Bytes{2,0x55,0xaa});link->request(19,Bytes{2});std::cout<<"PASS UART2 driver 115200 8E2 (no external peer)"<<std::endl;
 for(int mode=0;mode<4;mode++)for(int lsb=0;lsb<2;lsb++){
  link->request(17,cfg(5,1000000,mode,lsb));Bytes tx(240,0xa5);auto rx=([&]{Bytes packet{5};packet.insert(packet.end(),tx.begin(),tx.end());return link->request(18,packet);}());if(rx.size()!=tx.size())throw std::runtime_error("SPI length mismatch");
 }
 std::cout<<"PASS SPI 4 modes x 2 bit orders, 240-byte transfer (MISO unconnected)"<<std::endl;
 link->request(17,cfg(4,100000));Bytes found;for(int start=8;start<=0x77;start+=16){uint8_t range[]={4,(uint8_t)start,(uint8_t)std::min(start+15,0x77)};auto rx=link->request(20,range);found.insert(found.end(),rx.begin(),rx.end());}if(!found.empty())throw std::runtime_error("Unexpected I2C ACK on empty header");std::cout<<"PASS I2C scan empty bus; no false ACK"<<std::endl;
 link->request(17,cfg(0,0));link->close();link.reset();std::this_thread::sleep_for(std::chrono::seconds(2));link=connect();b=link->request(21);status.assign(b.begin(),b.end());if(json_int(status,"enabled_mask")!=0)throw std::runtime_error("Interface cleanup failed");link->close();CloseHandle(serial);serial=INVALID_HANDLE_VALUE;
 std::cout<<"RESULT: PASS UART1 actual wire test plus UART2/SPI/I2C driver tests; no target flash"<<std::endl;return 0;
}catch(const winrt::hresult_error&e){std::cerr<<"FAIL "<<utf8(e.message().c_str())<<std::endl;}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;}if(serial!=INVALID_HANDLE_VALUE)CloseHandle(serial);return 1;}
