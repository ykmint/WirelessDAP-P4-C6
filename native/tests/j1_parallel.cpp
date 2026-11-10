#include "../src/core.h"
#include "../src/board_config.h"
#include <winrt/base.h>
#include <iostream>
static Bytes cfg(int m,unsigned hz,int a=0,int b=0,int c=0){Bytes p{(uint8_t)m};put32(p,hz);p.push_back(a);p.push_back(b);p.push_back(c);return p;}
int wmain(int argc,wchar_t**argv){HANDLE serial=INVALID_HANDLE_VALUE;std::string stage="connect";try{
 if(argc<2)throw std::runtime_error("Specify the verified USB TTL COM port");
 winrt::init_apartment(winrt::apartment_type::multi_threaded);WSADATA ws{};WSAStartup(MAKEWORD(2,2),&ws);
 serial=CreateFileW((std::wstring(L"\\\\.\\")+argv[1]).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(serial==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open USB TTL serial port");
 DCB d{sizeof(d)};if(!GetCommState(serial,&d))throw std::runtime_error("GetCommState failed");d.BaudRate=115200;d.ByteSize=8;d.Parity=NOPARITY;d.StopBits=ONESTOPBIT;d.fBinary=TRUE;d.fParity=FALSE;d.fOutxCtsFlow=FALSE;d.fOutxDsrFlow=FALSE;d.fDtrControl=DTR_CONTROL_DISABLE;d.fRtsControl=RTS_CONTROL_DISABLE;d.fOutX=FALSE;d.fInX=FALSE;d.fAbortOnError=FALSE;if(!SetCommState(serial,&d))throw std::runtime_error("SetCommState failed");COMMTIMEOUTS t{MAXDWORD,0,50,0,1000};if(!SetCommTimeouts(serial,&t))throw std::runtime_error("SetCommTimeouts failed");PurgeComm(serial,PURGE_RXCLEAR|PURGE_TXCLEAR);
 auto connect=[](){return connect_ble(0xE83DC1D25042ULL,BOARD_TOKEN);};auto link=connect();auto b=link->request(3);std::string status(b.begin(),b.end());std::cout<<status<<std::endl;int mv=json_int(status,"vref_mv");if(mv<400&&argc>2&&std::wstring(argv[2])==L"power"){uint8_t on=1;link->request(4,{&on,1});b=link->request(3);status.assign(b.begin(),b.end());mv=json_int(status,"vref_mv");}if(mv<2700||mv>3600)throw std::runtime_error("Connect the target rail to J1-3 VREF first");
 link->request(17,cfg(1,115200,0,8,1));
 link->request(17,cfg(2,115200,0,8,1));link->request(17,cfg(3,115200,0,8,1));
 link->request(17,cfg(4,100000));link->request(17,cfg(5,1000000));link->request(17,cfg(6,500000,2));
 {auto data=link->request(21);std::string status(data.begin(),data.end());std::cout<<status<<std::endl;if(json_int(status,"enabled_mask")!=126)throw std::runtime_error("All six interfaces must remain enabled");}
 std::cout<<"PASS all six interfaces enabled together"<<std::endl;
 for(int round=0;round<3;round++){
  Bytes tx(96);for(size_t i=0;i<tx.size();i++)tx[i]=(uint8_t)(i*17+round*43);DWORD written=0;if(!WriteFile(serial,tx.data(),(DWORD)tx.size(),&written,nullptr)||written!=tx.size())throw std::runtime_error("USB TTL write failed");
  // While bytes arrive on UART1, operate SPI and CAN using the same BLE session.
  stage="parallel SPI";Bytes spi(241,0xa5);spi[0]=5;auto spiRx=link->request(18,spi);if(spiRx.size()!=240)throw std::runtime_error("Parallel SPI length mismatch");
  Bytes frame{6};put32(frame,0x123+round);frame.push_back(0);frame.push_back(8);for(int i=0;i<8;i++)frame.push_back(i+round);
  stage="CAN loopback";link->request(18,frame);auto can=link->request(19,Bytes{6});Bytes expected(frame.begin()+1,frame.end());if(can!=expected)throw std::runtime_error("Parallel CAN loopback mismatch");
  stage="RS485 TX";link->request(18,Bytes{3,0x55,0xaa});
  stage="I2C probe";auto scan=link->request(20,Bytes{4,0x50,0x50});if(!scan.empty())throw std::runtime_error("Unexpected I2C ACK");
  stage="UART1 RX/TX";Bytes rx;auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(rx.size()<tx.size()&&std::chrono::steady_clock::now()<until){auto q=link->request(19,Bytes{1});rx.insert(rx.end(),q.begin(),q.end());std::this_thread::sleep_for(std::chrono::milliseconds(20));}if(rx!=tx){std::cout<<"UART received="<<rx.size()<<" expected="<<tx.size()<<" data=";for(auto c:rx)std::cout<<std::hex<<unsigned(c)<<" ";std::cout<<std::dec<<std::endl;throw std::runtime_error("USB TTL TX -> J1 RX1 -> BLE byte mismatch");}
  tx.resize(240);for(size_t i=0;i<tx.size();i++)tx[i]=(uint8_t)(i*19+round*41);([&]{Bytes packet{1};packet.insert(packet.end(),tx.begin(),tx.end());return link->request(18,packet);}());rx.clear();until=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(rx.size()<tx.size()&&std::chrono::steady_clock::now()<until){uint8_t buf[256];DWORD got=0;if(!ReadFile(serial,buf,sizeof(buf),&got,nullptr))throw std::runtime_error("USB TTL read failed");rx.insert(rx.end(),buf,buf+got);}if(rx!=tx)throw std::runtime_error("BLE -> J1 TX1 -> USB TTL RX byte mismatch");
  std::cout<<"PASS parallel UART1 + SPI + CAN + RS485 + I2C round "<<round+1<<" (96 RX + 240 TX bytes)"<<std::endl;
 }
 // Keep UART bytes arriving while BLE SPI transactions run.
 Bytes stream(1600);for(size_t i=0;i<stream.size();i++)stream[i]=(uint8_t)(i*31+7);
 std::atomic<bool> streamOk{true};
 std::jthread writer([&]{for(size_t off=0;off<stream.size();off+=32){DWORD n=0;if(!WriteFile(serial,stream.data()+off,32,&n,nullptr)||n!=32){streamOk=false;return;}std::this_thread::sleep_for(std::chrono::milliseconds(20));}});
 for(int i=0;i<8;i++){Bytes p(241,0x5a);p[0]=5;if(link->request(18,p).size()!=240)throw std::runtime_error("Streaming SPI length mismatch");}
 writer.join();if(!streamOk)throw std::runtime_error("Streaming serial write failed");
 Bytes streamRx;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(streamRx.size()<stream.size()&&std::chrono::steady_clock::now()<deadline){auto q=link->request(19,Bytes{1});streamRx.insert(streamRx.end(),q.begin(),q.end());}
 if(streamRx!=stream)throw std::runtime_error("Continuous UART stream during SPI mismatch");
 auto uartStatus=link->request(21,Bytes{1});if(json_int(std::string(uartStatus.begin(),uartStatus.end()),"rx_errors")!=0)throw std::runtime_error("UART errors during concurrent stream");
 std::cout<<"PASS continuous UART1 1600 bytes during eight SPI transfers; UART errors 0"<<std::endl;
 link->request(17,cfg(2,115200,2,8,2));link->request(18,Bytes{2,0x55,0xaa});link->request(19,Bytes{2});std::cout<<"PASS UART2 driver 115200 8E2 (no external peer)"<<std::endl;
 for(int mode=0;mode<4;mode++)for(int lsb=0;lsb<2;lsb++){
  link->request(17,cfg(5,1000000,mode,lsb));Bytes tx(240,0xa5);auto rx=([&]{Bytes packet{5};packet.insert(packet.end(),tx.begin(),tx.end());return link->request(18,packet);}());if(rx.size()!=tx.size())throw std::runtime_error("SPI length mismatch");
 }
 std::cout<<"PASS SPI 4 modes x 2 bit orders, 240-byte transfer (MISO unconnected)"<<std::endl;
 stage="full I2C scan";link->request(17,cfg(4,100000));Bytes found;for(int start=8;start<=0x77;start+=16){uint8_t range[]={4,(uint8_t)start,(uint8_t)std::min(start+15,0x77)};std::cout<<"I2C scan range "<<start<<".."<<(int)range[2]<<std::endl;auto rx=link->request(20,range);found.insert(found.end(),rx.begin(),rx.end());}if(!found.empty()){std::cout<<"Unexpected I2C addresses:";for(auto address:found)std::cout<<" "<<std::hex<<unsigned(address);std::cout<<std::dec<<std::endl;throw std::runtime_error("Unexpected I2C ACK on empty header");}std::cout<<"PASS I2C scan empty bus; no false ACK"<<std::endl;
 // An absent I2C peer must not tear down healthy concurrent UARTs.
 bool nack=false;try{link->request(18,Bytes{4,0x50,1});}catch(const InterfaceError&e){if(std::string(e.what()).find("ESP_ERR_INVALID_RESPONSE")==std::string::npos)throw;nack=true;}if(!nack)throw std::runtime_error("Expected I2C NACK");
 {auto st=link->request(21);if(json_int(std::string(st.begin(),st.end()),"enabled_mask")!=126)throw std::runtime_error("I2C error closed unrelated interfaces");}
 link->request(22,Bytes{5});{auto st=link->request(21);if(json_int(std::string(st.begin(),st.end()),"enabled_mask")!=94)throw std::runtime_error("Per-interface close changed other ports");}
 Bytes dap(64);dap[1]=2;auto dap_reply=link->request(2,dap);if(dap_reply.size()<3||dap_reply[0]!=0)throw std::runtime_error("Concurrent DAP_Info failed");
 std::cout<<"PASS I2C NACK isolation, single-port close and concurrent DAP_Info"<<std::endl;
 link->request(17,cfg(0,0));link->disconnect();link.reset();std::this_thread::sleep_for(std::chrono::seconds(2));link=connect();b=link->request(21);status.assign(b.begin(),b.end());if(json_int(status,"enabled_mask")!=0)throw std::runtime_error("Interface cleanup failed");link->disconnect();CloseHandle(serial);serial=INVALID_HANDLE_VALUE;
 std::cout<<"RESULT: PASS parallel J1 actual UART1 + CAN self-loop + SPI/I2C/RS485 drivers; no target flash"<<std::endl;return 0;
}catch(const winrt::hresult_error&e){std::cerr<<"FAIL "<<utf8(e.message().c_str())<<std::endl;}catch(const std::exception&e){std::cerr<<"FAIL ["<<stage<<"] "<<e.what()<<std::endl;}if(serial!=INVALID_HANDLE_VALUE)CloseHandle(serial);return 1;}
