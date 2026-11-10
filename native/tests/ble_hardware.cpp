#include "../src/core.h"
#include "../src/board_config.h"
#include <winrt/base.h>
#include <iostream>
int main(){
 try{
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  WSADATA ws{};WSAStartup(MAKEWORD(2,2),&ws);
  auto found=scan_ble();if(found.empty())throw std::runtime_error("No WirelessDAP advertisement");
  auto it=std::find_if(found.begin(),found.end(),[](auto& d){return d.address==0xE83DC1D25042ULL;});
  if(it==found.end())throw std::runtime_error("Expected board not found");
  std::cout<<"PASS native BLE scan "<<std::hex<<it->address<<std::dec<<std::endl;
  for(int round=0;round<3;round++){
   auto link=connect_ble(it->address,BOARD_TOKEN);
   std::cout<<"PASS GATT connect and authentication round "<<round+1<<std::endl;
   for(int i=0;i<20;i++){
    auto reply=link->request(3);std::string status(reply.begin(),reply.end());
    if(json_int(status,"max_swd_hz")!=100000)throw std::runtime_error("Invalid board status");
    if(i==0)std::cout<<"Board status "<<status<<std::endl;
    Bytes req(64,0);req[1]=2; // DAP_Info product: no target GPIO access
    auto dap=link->request(2,req);
    if(dap.size()<3||dap[0]!=0||dap[1]+2!=dap.size())throw std::runtime_error("Invalid DAP_Info");
    std::string product(dap.begin()+2,dap.end());
    if(product.find("P4-C6 SWD")==std::string::npos)throw std::runtime_error("Unexpected DAP product");
    if(i==0)std::cout<<"DAP product "<<product.c_str()<<std::endl;
   }
   std::cout<<"PASS 20 status + 20 fragmented DAP exchanges"<<std::endl;
   link->close();link.reset();std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  std::cout<<"RESULT: PASS native BLE hardware transport; no target flash attempted"<<std::endl;
  WSACleanup();return 0;
 }catch(const winrt::hresult_error&e){std::cerr<<"FAIL WinRT "<<utf8(e.message().c_str())<<std::endl;return 1;}
 catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
}
