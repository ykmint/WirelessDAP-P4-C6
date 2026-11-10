#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#pragma comment(lib,"cabinet.lib")
namespace fs=std::filesystem;using B=std::vector<unsigned char>;
void u16(B&b,unsigned v){b.push_back(v&255);b.push_back(v>>8);}
void u32(B&b,unsigned v){for(int i=0;i<4;i++)b.push_back((v>>(8*i))&255);}
int wmain(int n,wchar_t**a){if(n!=3)return 2;try{fs::path root=a[1];std::vector<fs::path> files;for(auto&e:fs::recursive_directory_iterator(root))if(e.is_regular_file())files.push_back(e.path());B raw;u32(raw,(unsigned)files.size());for(auto&p:files){std::ifstream f(p,std::ios::binary);B data((std::istreambuf_iterator<char>(f)),{});auto u8=fs::relative(p,root).generic_u8string();std::string name((char*)u8.data(),u8.size());u16(raw,(unsigned)name.size());u32(raw,(unsigned)data.size());raw.insert(raw.end(),name.begin(),name.end());raw.insert(raw.end(),data.begin(),data.end());}COMPRESSOR_HANDLE c; if(!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&c))return 3;SIZE_T size=0;Compress(c,raw.data(),raw.size(),nullptr,0,&size);B compressed(size);if(!Compress(c,raw.data(),raw.size(),compressed.data(),compressed.size(),&size))return 4;CloseCompressor(c);compressed.resize(size);B out;u32(out,0x32424457);u32(out,(unsigned)raw.size());out.insert(out.end(),compressed.begin(),compressed.end());std::ofstream f(a[2],std::ios::binary);f.write((char*)out.data(),out.size());std::cout<<files.size()<<" files, "<<raw.size()<<" -> "<<out.size()<<" bytes\n";return !f;}catch(std::exception&e){std::cerr<<e.what();return 5;}}
