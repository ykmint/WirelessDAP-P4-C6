#include "stlink_bridge.h"
#include "core.h"
#include "ssh.h"
#include <map>
class MockLink final:public Link{
public:
 bool closed=false,badSequence=false;int count=0;Bytes last;
 void close()noexcept override{closed=true;}
 Bytes exchange(std::span<const uint8_t>b)override{
 count++;last=Bytes(b.begin(),b.end());if(b.size()<10||u16(b.data()+8)!=b.size()-10)throw std::runtime_error("Bad test request");Bytes out{'W','D',1,uint8_t(b[3]|128)};put32(out,u32(b.data()+4)+(badSequence?1:0));Bytes data{0};auto p=b.subspan(10);
 if(b[3]==1){if(std::string(p.begin(),p.end())!="test")data[0]=1;}
 else if(b[3]==3){std::string s="{\"vref_mv\":3300,\"fault\":0}";data.insert(data.end(),s.begin(),s.end());}
 else if(b[3]==2){if(p.size()!=64)throw std::runtime_error("DAP padding mismatch");uint8_t cmd=p[0];Bytes r;
 if(cmd==0){Bytes info;switch(p[1]){case 1:info={'T','e','s','t',0};break;case 2:info={'W','i','r','e','l','e','s','s','D','A','P',0};break;case 3:info={'T','E','S','T',0};break;case 4:info={'2','.','1','.','2',0};break;case 9:info={'2','.','0',0};break;case 0xf0:info={1,0};break;case 0xfe:info={1};break;case 0xff:info={64,0};break;}r={0,uint8_t(info.size())};r.insert(r.end(),info.begin(),info.end());}
 else if(cmd==2)r={2,1};else if(cmd==0x10)r={cmd,0x80};else if(cmd==5){r={cmd,p[2],1};size_t off=3;for(unsigned i=0;i<p[2];i++){uint8_t req=p[off++];if(req&2)put32(r,req==2?0x2ba01477:0xf0000000);else off+=4;}}else r={cmd,0};data.insert(data.end(),r.begin(),r.end());}
 put16(out,(uint16_t)data.size());out.insert(out.end(),data.begin(),data.end());return out;
 }
};
void Engine::self_test(Log log){auto mock=std::make_shared<MockLink>();mock->authenticate("test");auto dir=assets/L"protocol-test";fs::create_directories(dir);bool found=false;int code=execute(mock,"gdb port disabled\ntcl port disabled\ntelnet port disabled\ninit\ncmsis-dap info\necho WDP_NATIVE_ENGINE_OK\nshutdown\n",dir,[&](const std::wstring&s){if(s.find(L"WDP_NATIVE_ENGINE_OK")!=s.npos)found=true;log(s);},30);if(code||!found||mock->count<5)throw std::runtime_error("Real OpenOCD / C++ bridge integration failed, exit="+std::to_string(code)+", requests="+std::to_string(mock->count));for(size_t i=1;i<profiles.size();i++){
 std::string config="source [find target/"+std::string(profiles[i].script)+"]\nadapter speed 100\nreset_config none\ncortex_m reset_config sysresetreq\necho WDP_CONFIG_OK\nshutdown\n";
 bool valid=false;int result=execute(mock,config,dir,[&](const std::wstring&s){if(s.find(L"WDP_CONFIG_OK")!=s.npos)valid=true;},15);
 if(result||!valid)throw std::runtime_error("Target configuration invalid: "+std::string(profiles[i].script));
}
log(L"All 22 STM32 family configurations parsed by native engine (no target attached).");}
static void tcp_fragment_test(){
 SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(listener==INVALID_SOCKET||bind(listener,(sockaddr*)&a,sizeof(a))||listen(listener,1))throw std::runtime_error("TCP test listener failed");int len=sizeof(a);getsockname(listener,(sockaddr*)&a,&len);std::exception_ptr failure;std::atomic<SOCKET> peer{INVALID_SOCKET};
 std::thread server([&]{SOCKET c=accept(listener,nullptr,nullptr);peer=c;try{if(c==INVALID_SOCKET)throw std::runtime_error("Test accept failed");DWORD timeout=5000;setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));MockLink device;
 auto get=[&](uint8_t*p,size_t n){while(n){int r=recv(c,(char*)p,(int)n,0);if(r<=0)throw std::runtime_error("Test receive failed");p+=r;n-=r;}};
 for(int i=0;i<3;i++){Bytes b(10);get(b.data(),10);size_t n=u16(b.data()+8);if(n>256)throw std::runtime_error("Test frame bound");b.resize(10+n);get(b.data()+10,n);auto reply=device.exchange(b);for(size_t off=0;off<reply.size();){int sent=send(c,(char*)reply.data()+off,(int)std::min<size_t>(3,reply.size()-off),0);if(sent<=0)throw std::runtime_error("Test send failed");off+=sent;}}}catch(...){failure=std::current_exception();}if(c!=INVALID_SOCKET){shutdown(c,SD_BOTH);closesocket(c);}peer=INVALID_SOCKET;});
 try{auto client=connect_tcp(L"127.0.0.1",ntohs(a.sin_port),"test");auto status=client->request(3);if(json_int(std::string(status.begin(),status.end()),"vref_mv")!=3300)throw std::runtime_error("TCP status mismatch");Bytes request(64);request[1]=0xff;auto answer=client->request(2,request);if(answer!=Bytes({0,2,64,0}))throw std::runtime_error("TCP DAP response mismatch");client->close();}
 catch(...){if(peer!=INVALID_SOCKET)shutdown(peer,SD_BOTH);closesocket(listener);server.join();throw;}server.join();closesocket(listener);if(failure)std::rethrow_exception(failure);
}
static void require(bool p,const char*msg){if(!p)throw std::runtime_error(msg);}
bool self_test(const fs::path&report,Log log){std::ostringstream text;int passed=0;try{
 auto check=[&](const char*name,std::function<void()>fn){fn();passed++;text<<"PASS "<<name<<"\n";log(wide(name));};
 auto assets=unpack_assets();auto dir=assets/L"native-tests";fs::create_directories(dir);
 check("SSH terminal runtime, injection guard and cancellation",[&]{ssh_self_test(assets,log);});
 check("ST-LINK API3 framing, memory widths and failure isolation",[&]{stlink_self_test(log);});
 check("Embedded OpenOCD ST-LINK TCP integration",[&]{stlink_engine_self_test(assets,[&](const std::wstring& line){text<<utf8(line)<<"\n";log(line);});});
 check("embedded engine available",[&]{require(fs::exists(assets/L"openocd.exe"),"engine missing");});
 check("offline STM32 profiles complete",[&]{require(profiles.size()==23,"profile count");for(size_t i=1;i<profiles.size();i++)require(fs::exists(assets/L"scripts/target"/profiles[i].script),"profile missing");});
 check("authentication and status",[]{MockLink m;m.authenticate("test");auto b=m.request(3);require(json_int(std::string(b.begin(),b.end()),"vref_mv")==3300,"status mismatch");});
 check("wrong token closes session",[]{MockLink m;bool rejected=false;try{m.authenticate("bad");}catch(...){rejected=true;}require(rejected&&m.closed&&m.count==1,"auth not fail closed");});
 check("wrong sequence closes without replay",[]{MockLink m;m.badSequence=true;bool rejected=false;try{m.authenticate("test");}catch(...){rejected=true;}require(rejected&&m.closed&&m.count==1,"sequence/retry failure");});
 check("payload upper bound",[]{MockLink m;bool rejected=false;try{m.request(2,Bytes(257));}catch(...){rejected=true;}require(rejected&&m.count==0,"payload not bounded");});
 check("binary image address",[&]{auto p=dir/L"app.bin";write_file(p,Bytes{1,2,3,4});auto s=parse_image(p,0x08000000);require(s.size()==1&&s[0].bytes.size()==4,"bin parse");});
 check("empty image rejected",[&]{auto p=dir/L"empty.bin";write_file(p,{});bool rejected=false;try{parse_image(p,0x08000000);}catch(...){rejected=true;}require(rejected,"empty accepted");});
 check("RAM image rejected before programming",[&]{bool rejected=false;try{parse_image(dir/L"app.bin",0x20000000);}catch(...){rejected=true;}require(rejected,"RAM accepted");});
 check("Intel HEX checksum and addresses",[&]{std::string h=":020000040800F2\n:0400000001020304F2\n:00000001FF\n";auto p=dir/L"app.hex";write_file(p,{(uint8_t*)h.data(),h.size()});auto s=parse_image(p,0);require(s.size()==1&&s[0].address==0x08000000&&s[0].bytes==Bytes({1,2,3,4}),"HEX parse");});
 check("corrupt HEX rejected",[&]{std::string h=":020000040800F3\n:00000001FF\n";auto p=dir/L"bad.hex";write_file(p,{(uint8_t*)h.data(),h.size()});bool rejected=false;try{parse_image(p,0);}catch(...){rejected=true;}require(rejected,"bad HEX accepted");});
 check("overlap and range overflow rejected",[]{std::vector<Segment>s{{0x08000000,{1,2,3}},{0x08000002,{4}}};bool rejected=false;try{validate_segments(s);}catch(...){rejected=true;}require(rejected,"overlap accepted");});
 check("non-ARM ELF rejected",[&]{auto p=dir/L"bad.elf";write_file(p,Bytes(80));bool rejected=false;try{parse_image(p,0);}catch(...){rejected=true;}require(rejected,"bad ELF accepted");});
 check("real Winsock transport with fragmented replies",[]{tcp_fragment_test();});
 check("Keil installer preserves existing drivers and is idempotent",[&]{
   auto root=dir/L"keil-installer-fixture";
   std::string original="[ARM]\r\nCPUDLL1=SARMCM3.DLL(TDRV1)\r\nCPUDLL3=SARMV8M.DLL(TDRV1)\r\nTDRV900=BIN\\existing.dll(\"Existing\")\r\n";
   write_file(root/L"TOOLS.INI",{(const uint8_t*)original.data(),original.size()});
   write_file(root/L"UV4/UV4.exe",Bytes{0});
   install_keil_driver(root,assets);
   auto first=read_file(root/L"TOOLS.INI");std::string result(first.begin(),first.end());
   require(result.find("existing.dll")!=result.npos&&result.find("elaphureLink Debugger")!=result.npos,"Keil registry update failed");
   require(read_file(root/L"ARM/BIN/elaphureRddi.dll")==read_file(assets/L"keil/elaphureRddi.dll"),"Keil DLL copy mismatch");
   install_keil_driver(root,assets);require(read_file(root/L"TOOLS.INI")==first,"Keil installation is not idempotent");
 });
 check("OpenOCD native TCP bridge integration",[&]{Engine e(assets);e.self_test([&](const std::wstring&s){text<<utf8(s)<<"\n";});});
 text<<"\nRESULT: "<<passed<<" PASS\nNo physical STM32 / Wi-Fi / BLE flashing performed in this test.\n";write_file(report,{(const uint8_t*)text.str().data(),text.str().size()});return true;
 }catch(const std::exception&e){text<<"FAIL: "<<e.what()<<"\n";auto t=text.str();try{write_file(report,{(uint8_t*)t.data(),t.size()});}catch(...){}return false;}}
