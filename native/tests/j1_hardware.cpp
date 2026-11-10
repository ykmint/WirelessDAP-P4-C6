// Historical API v1 bench test. Use j1_parallel.cpp for current API v2 acceptance.
#include "../src/core.h"
#include "../src/board_config.h"
#include <winrt/base.h>
#include <iostream>
static constexpr uint64_t board=0xE83DC1D25042ULL;
static Bytes config(int mode,unsigned hz,int a=0,int b=0,int c=0){Bytes p{(uint8_t)mode};put32(p,hz);p.push_back(a);p.push_back(b);p.push_back(c);return p;}
int main(){try{
 winrt::init_apartment(winrt::apartment_type::multi_threaded);WSADATA ws{};WSAStartup(MAKEWORD(2,2),&ws);
 auto link=connect_ble(board,BOARD_TOKEN);
 auto show=[&](uint8_t op){auto b=link->request(op);std::string s(b.begin(),b.end());std::cout<<s<<std::endl;return s;};
 if(json_int(show(16),"j1_api")!=1)throw std::runtime_error("Wrong J1 API");
 if(json_int(show(3),"vref_mv")>100)throw std::runtime_error("Expected empty J1 for bench test");
 link->request(17,config(0,0));std::cout<<"PASS interface capability and idle state"<<std::endl;
 link->request(17,config(3,115200,0,8,1));if(json_int(show(21),"mode")!=3)throw std::runtime_error("RS485 config mismatch");
 link->request(18,Bytes{0x55,0xaa,0x00,0xff});link->request(19);std::cout<<"PASS RS485 configure, TX completion, RX polling (no external peer)"<<std::endl;
 link->request(17,config(6,500000,2));if(json_int(show(21),"mode")!=6)throw std::runtime_error("CAN config mismatch");
 for(int extended=0;extended<2;extended++){
  Bytes frame;put32(frame,extended?0x1abcde:0x123);frame.push_back(extended);frame.push_back(8);for(int i=0;i<8;i++)frame.push_back(i*31);
  link->request(18,frame);Bytes got;for(int i=0;i<10&&got.empty();i++){std::this_thread::sleep_for(std::chrono::milliseconds(40));got=link->request(19);}
  if(got!=frame)throw std::runtime_error("CAN loopback mismatch");
 }
 std::cout<<"PASS CAN classic standard and extended self-test loopback"<<std::endl;
 link->request(17,config(0,0));link->close();link.reset();std::this_thread::sleep_for(std::chrono::seconds(2));
 for(int m:{1,2,4,5}){
  link=connect_ble(board,BOARD_TOKEN);bool denied=false;
  try{link->request(17,config(m,m==5?1000000:100000,0,m<=2?8:0,m<=2?1:0));}catch(const std::exception&e){denied=std::string(e.what()).find("ESP_ERR_INVALID_STATE")!=std::string::npos;}
  link->close();link.reset();if(!denied)throw std::runtime_error("Missing VREF did not block TTL interface");std::cout<<"PASS missing VREF guard mode "<<m<<std::endl;std::this_thread::sleep_for(std::chrono::seconds(2));
 }
 link=connect_ble(board,BOARD_TOKEN);if(json_int(show(21),"mode")!=0)throw std::runtime_error("Disconnect cleanup failed");
 Bytes dap(64);dap[1]=2;auto info=link->request(2,dap);if(info.size()<3||info[0]!=0)throw std::runtime_error("SWD DAP regression");std::cout<<"PASS SWD DAP_Info after interface cleanup"<<std::endl;
 link->close();std::cout<<"RESULT: PASS J1 bench tests; external UART/I2C/SPI/RS485/CAN peers not tested"<<std::endl;return 0;
}catch(const winrt::hresult_error&e){std::cerr<<"FAIL "<<utf8(e.message().c_str())<<std::endl;return 1;}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
