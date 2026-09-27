/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "RawCapture.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(0)
int main() {
    auto dir = (std::filesystem::temp_directory_path()/"stream1090-capture-XXXXXX").string();
    CHECK(mkdtemp(dir.data()));
    const auto path=dir+"/test.cs16";
    {
        RawCapture c(path,4000000,3,{{"serial","quote\"\n"}});
        int16_t data[]={-32768,32767,1,-1,2,-2,3,-3};
        c.append(data,8); CHECK(c.done()); CHECK(c.samples()==3);
        c.append(data,8); CHECK(c.samples()==3);
        c.finish(true,"complete");
    }
    CHECK(std::filesystem::file_size(path)==12);
    std::ifstream in(path,std::ios::binary);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in),{}};
    CHECK(bytes[0]==0 && bytes[1]==128 && bytes[2]==255 && bytes[3]==127);
    bool rejected=false; try { RawCapture c(path,4000000,3,{}); } catch (...) { rejected=true; }
    CHECK(rejected); CHECK(std::filesystem::file_size(path)==12);
    std::ifstream meta(path+".json"); std::string text{std::istreambuf_iterator<char>(meta),{}};
    CHECK(text.find("\"valid\": true")!=std::string::npos);
    CHECK(text.find("quote\\\"\\u000a")!=std::string::npos);
    { RawCapture aborted(dir+"/aborted",4000000,3,{}); }
    std::ifstream incomplete(dir+"/aborted.json"); text.assign(std::istreambuf_iterator<char>(incomplete),{});
    CHECK(text.find("\"valid\": false")!=std::string::npos);
    {
        RawCapture complete(dir+"/prefix",4000000,2,{});
        int16_t data[]={1,2,3,4}; complete.append(data,4);
        CHECK(complete.finishSession(false)); // Failure occurred after this prefix.
    }
    std::ifstream prefix(dir+"/prefix.json"); text.assign(std::istreambuf_iterator<char>(prefix),{});
    CHECK(text.find("complete_before_device_failure")!=std::string::npos);
    CHECK(text.find("\"valid\": true")!=std::string::npos);
    {
        RawCapture shortRun(dir+"/short",4000000,3,{});
        int16_t data[]={1,2}; shortRun.append(data,2);
        CHECK(!shortRun.finishSession(false)); // Never bless a missing suffix.
    }
    std::filesystem::remove_all(dir);
}
