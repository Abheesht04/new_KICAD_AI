#include "studio.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif
namespace fs=std::filesystem;
static std::string read_file(const char *p) {std::ifstream f(p,std::ios::binary);std::ostringstream s;s<<f.rdbuf();return s.str();}
static std::string run_process(const std::vector<std::string>& args) {
#ifdef _WIN32
    auto quote=[](const std::string& s){std::string o="\"";unsigned slashes=0;for(char c:s){if(c=='\\'){slashes++;continue;}if(c=='"'){o.append(slashes*2+1,'\\');o+=c;}else{o.append(slashes,'\\');o+=c;}slashes=0;}o.append(slashes*2,'\\');return o+'"';};
    std::string cmd;for(auto &s:args){if(!cmd.empty())cmd+=' ';cmd+=quote(s);}
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE r=nullptr,w=nullptr;
    if(!CreatePipe(&r,&w,&sa,0))return "Could not create process pipe";
    SetHandleInformation(r,HANDLE_FLAG_INHERIT,0);STARTUPINFOA si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=w;si.hStdError=w;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);PROCESS_INFORMATION pi{};
    BOOL ok=CreateProcessA(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,COPPER_ROOT,&si,&pi);CloseHandle(w);
    if(!ok){CloseHandle(r);return "Launch failed. Install Python and add it to PATH, or set HWC_PYTHON.";}
    std::string out;char buf[4096];DWORD n;while(ReadFile(r,buf,sizeof(buf),&n,nullptr)&&n){if(out.size()<4*1024*1024)out.append(buf,n);}CloseHandle(r);WaitForSingleObject(pi.hProcess,INFINITE);DWORD rc;GetExitCodeProcess(pi.hProcess,&rc);CloseHandle(pi.hProcess);CloseHandle(pi.hThread);return out+"\nProcess exit: "+std::to_string(rc);
#else
    int fds[2];if(pipe(fds))return "Pipe failed";std::vector<char*> av;for(auto&s:args)av.push_back(const_cast<char*>(s.c_str()));av.push_back(nullptr);
    pid_t pid=fork();if(pid==0){close(fds[0]);dup2(fds[1],1);dup2(fds[1],2);close(fds[1]);execvp(av[0],av.data());_exit(127);}close(fds[1]);if(pid<0){close(fds[0]);return "Fork failed";}
    std::string out;char buf[4096];ssize_t n;while((n=read(fds[0],buf,sizeof buf))>0)if(out.size()<4*1024*1024)out.append(buf,(size_t)n);close(fds[0]);int rc=0;while(waitpid(pid,&rc,0)<0&&errno==EINTR){}return out+"\nProcess exit: "+std::to_string(WIFEXITED(rc)?WEXITSTATUS(rc):128);
#endif
}
Studio::Studio(const char *path):editor_(256*1024,0) {
    std::snprintf(out_,sizeof out_,"%s/out/studio",COPPER_ROOT);
    auto initial=*path?std::string(path):std::string(COPPER_ROOT)+"/examples/v2/divider.copper";
    load_file(initial.c_str());log_="Open a project, inspect its components, then Analyze or Build.\nBuild resolves your local KiCad libraries and runs physical checks.\nThe canvas shows actual parsed placements and generated copper; it is not a fabrication approval.";
}
Studio::~Studio(){if(job_.valid())job_.wait();if(project_){cp_free(project_);delete project_;}}
void Studio::theme(){auto&s=ImGui::GetStyle();s.WindowRounding=0;s.FrameRounding=5;s.GrabRounding=5;s.ChildRounding=7;s.FramePadding={10,7};s.ItemSpacing={10,9};s.WindowPadding={18,16};s.Colors[ImGuiCol_WindowBg]={.047f,.059f,.078f,1};s.Colors[ImGuiCol_ChildBg]={.064f,.079f,.101f,1};s.Colors[ImGuiCol_FrameBg]={.10f,.12f,.15f,1};s.Colors[ImGuiCol_Button]={.13f,.20f,.23f,1};s.Colors[ImGuiCol_ButtonHovered]={.16f,.38f,.36f,1};s.Colors[ImGuiCol_Header]={.12f,.28f,.27f,1};s.Colors[ImGuiCol_Text]={.88f,.91f,.94f,1};s.Colors[ImGuiCol_Border]={.15f,.18f,.22f,1};}
void Studio::load_file(const char *path){std::string data=read_file(path);if(data.empty()||data.size()>=editor_.size()){status_="Cannot load file (empty, missing or over 256 KiB)";return;}std::snprintf(path_,sizeof path_,"%s",path);std::copy(data.begin(),data.end(),editor_.begin());editor_[data.size()]=0;dirty_=false;selected_=-1;refresh(path_);}
bool Studio::save_file(){std::ofstream f(path_,std::ios::binary);if(!f){status_="Save failed";return false;}f<<editor_.data();f.close();if(!f){status_="Save failed";return false;}dirty_=false;refresh(path_);status_="Saved";return true;}
void Studio::refresh(const char *path){if(project_){cp_free(project_);delete project_;}project_=new CpProject;cp_init(project_);char err[512];if(!cp_load(project_,path,err,sizeof err)){parse_error_=err;cp_free(project_);delete project_;project_=nullptr;}else parse_error_.clear();}
void Studio::launch(std::vector<std::string> args){if(job_.valid())return;const char*py=getenv("HWC_PYTHON");
#ifdef _WIN32
    if(!py)py="python";
#else
    if(!py)py="python3";
#endif
    std::vector<std::string> av={py,std::string(COPPER_ROOT)+"/tools/copper.py"};av.insert(av.end(),args.begin(),args.end());status_="Working...";job_=std::async(std::launch::async,[av]{return run_process(av);});}
void Studio::board(){
    ImVec2 size=ImGui::GetContentRegionAvail(),origin=ImGui::GetCursorScreenPos();size.y=std::max(size.y,150.f);ImGui::InvisibleButton("board_canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);auto*d=ImGui::GetWindowDrawList();d->PushClipRect(origin,{origin.x+size.x,origin.y+size.y},true);d->AddRectFilled(origin,{origin.x+size.x,origin.y+size.y},IM_COL32(14,21,28,255),8);
    if(ImGui::IsItemHovered()){zoom_=std::clamp(zoom_+ImGui::GetIO().MouseWheel*.08f,.35f,4.f);if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle)){pan_.x+=ImGui::GetIO().MouseDelta.x;pan_.y+=ImGui::GetIO().MouseDelta.y;}}
    if(!project_){d->AddText({origin.x+24,origin.y+24},IM_COL32(239,139,112,255),parse_error_.c_str());d->PopClipRect();return;}
    auto &b=project_->board;float scale=std::min((size.x-80)/(float)b.spec.width_mm,(size.y-80)/(float)b.spec.height_mm)*zoom_;ImVec2 base{origin.x+40+pan_.x,origin.y+40+pan_.y};auto point=[&](double x,double y){return ImVec2{base.x+(float)x*scale,base.y+(float)y*scale};};
    for(float x=origin.x;x<origin.x+size.x;x+=20)for(float y=origin.y;y<origin.y+size.y;y+=20)d->AddCircleFilled({x,y},.8f,IM_COL32(38,48,57,255));
    d->AddRectFilled(base,point(b.spec.width_mm,b.spec.height_mm),IM_COL32(20,47,43,255),4);d->AddRect(base,point(b.spec.width_mm,b.spec.height_mm),IM_COL32(100,180,160,255),4,0,1.5f);
    for(size_t i=0;i<vec_len(b.traces);i++){auto&t=b.traces[i];if((t.layer==0&&!top_)||(t.layer==2&&!bottom_))continue;d->AddLine(point(t.x1_mm,t.y1_mm),point(t.x2_mm,t.y2_mm),t.layer==0?IM_COL32(231,131,101,255):IM_COL32(99,155,225,255),std::max(1.f,(float)t.width_mm*scale));}
    for(size_t i=0;i<vec_len(b.pads);i++){auto&p=b.pads[i];auto center=point(p.x_mm,p.y_mm);if(p.rectangular)d->AddRectFilled(point(p.x_mm-p.width_mm/2,p.y_mm-p.height_mm/2),point(p.x_mm+p.width_mm/2,p.y_mm+p.height_mm/2),IM_COL32(229,199,116,255),2);else d->AddCircleFilled(center,(float)p.pad_diameter_mm*scale/2,IM_COL32(229,199,116,255));if(p.drill_mm>0)d->AddCircleFilled(center,(float)p.drill_mm*scale/2,IM_COL32(16,24,30,255));}
    for(size_t i=0;i<vec_len(b.vias);i++){auto&v=b.vias[i];d->AddCircleFilled(point(v.x_mm,v.y_mm),(float)v.diameter_mm*scale/2,IM_COL32(229,199,116,255));d->AddCircleFilled(point(v.x_mm,v.y_mm),(float)v.drill_mm*scale/2,IM_COL32(16,24,30,255));}
    for(size_t i=0;i<project_->component_count;i++){auto&c=project_->components[i];if(c.kind=='V'||c.kind=='I')continue;auto a=point(c.x,c.y);ImU32 color=selected_==(int)i?IM_COL32(111,236,202,255):IM_COL32(201,218,220,255);d->AddRect({a.x-16,a.y-10},{a.x+16,a.y+10},color,3);d->AddText({a.x-10,a.y-28},color,c.ref);if(ImGui::IsItemHovered()&&ImGui::IsMouseClicked(0)&&std::hypot(ImGui::GetIO().MousePos.x-a.x,ImGui::GetIO().MousePos.y-a.y)<22)selected_=(int)i;}
    d->AddText({origin.x+16,origin.y+size.y-25},IM_COL32(131,158,163,255),project_->has_geometry?"ACTUAL COPPER  /  Scroll to zoom  /  Middle-drag to pan":"PLACEMENT PREVIEW  /  Build to resolve footprints and route");d->PopClipRect();
}
void Studio::draw(){
    if(job_.valid()&&job_.wait_for(std::chrono::seconds(0))==std::future_status::ready){log_=job_.get();status_="Task complete - review output";auto resolved=fs::path(out_)/"resolved.copper";if(fs::exists(resolved))refresh(resolved.string().c_str());}
    auto*v=ImGui::GetMainViewport();ImGui::SetNextWindowPos(v->WorkPos);ImGui::SetNextWindowSize(v->WorkSize);ImGui::Begin("Copper Studio",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored({.45f,.9f,.77f,1},"C O P P E R");ImGui::SameLine();ImGui::TextDisabled("STUDIO   /   V2");ImGui::SameLine(ImGui::GetWindowWidth()-330);ImGui::TextDisabled("Deterministic electronics workbench");ImGui::Separator();
    bool busy=job_.valid();ImGui::BeginDisabled(busy);ImGui::SetNextItemWidth(std::max(180.f,ImGui::GetWindowWidth()-620));ImGui::InputText("##project",path_,sizeof path_);ImGui::SameLine();if(ImGui::Button("Open")&&!dirty_)load_file(path_);ImGui::SameLine();if(ImGui::Button(dirty_?"Save *":"Save"))save_file();ImGui::SameLine();if(ImGui::Button("Analyze")&&save_file())launch({"check",path_,"--core",COPPER_CORE});ImGui::SameLine();if(ImGui::Button("Build PCB")&&save_file())launch({"build",path_,"--out",out_,"--core",COPPER_CORE});ImGui::SameLine();if(ImGui::Button("KiCad gates"))launch({"verify","--out",out_,"--core",COPPER_CORE});ImGui::EndDisabled();
    if(dirty_)ImGui::TextColored({1,.72f,.4f,1},"Unsaved edits - save before opening another project.");
    ImGui::Spacing();float height=ImGui::GetContentRegionAvail().y-205;float right=275,left=225,center=std::max(300.f,ImGui::GetContentRegionAvail().x-left-right-20);
    ImGui::BeginChild("explorer",{left,height},ImGuiChildFlags_Borders);ImGui::TextDisabled("PROJECT");if(project_){ImGui::Text("%s",project_->name);ImGui::TextDisabled("%zu parts / %zu nets",project_->component_count,project_->net_count);ImGui::Separator();ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##filter","Filter components",filter_,sizeof filter_);for(size_t i=0;i<project_->component_count;i++){auto&c=project_->components[i];if(*filter_&&!strstr(c.ref,filter_)&&!strstr(c.part,filter_))continue;char label[256];std::snprintf(label,sizeof label,"%s   %s",c.ref,c.part);if(ImGui::Selectable(label,selected_==(int)i))selected_=(int)i;}ImGui::Separator();ImGui::TextDisabled("NETS");for(size_t i=0;i<project_->net_count;i++)ImGui::BulletText("%s",project_->nets[i]);}ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("workspace",{center,height},ImGuiChildFlags_Borders);if(ImGui::Button(show_source_?"Board canvas":"Project source"))show_source_=!show_source_;ImGui::SameLine();ImGui::Checkbox("F.Cu",&top_);ImGui::SameLine();ImGui::Checkbox("B.Cu",&bottom_);if(show_source_){if(ImGui::InputTextMultiline("##source",editor_.data(),editor_.size(),{-1,-1},ImGuiInputTextFlags_AllowTabInput))dirty_=true;}else board();ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("inspector",{right,height},ImGuiChildFlags_Borders);ImGui::TextDisabled("INSPECTOR");if(project_&&selected_>=0&&(size_t)selected_<project_->component_count){auto&c=project_->components[selected_];ImGui::TextColored({.45f,.9f,.77f,1},"%s",c.ref);ImGui::TextWrapped("%s",c.part);ImGui::Text("Value: %.8g SI",c.value);ImGui::TextWrapped("Symbol: %s",c.symbol);ImGui::TextWrapped("Footprint: %s",c.footprint);ImGui::Separator();for(size_t j=0;j<c.pin_count;j++)ImGui::Text("Pin %s  ->  %s",c.pins[j],project_->nets[c.nodes[j]]);ImGui::Separator();ImGui::Text("Power rating: %.6g W",c.power_w);ImGui::Text("Voltage rating: %.6g V",c.voltage_v);if(!c.power_w||!c.voltage_v)ImGui::TextWrapped("Zero means unknown. Supply ratings from a verified part datasheet in your catalogue.");}else ImGui::TextWrapped("Select a component in the explorer or canvas.");ImGui::Separator();ImGui::TextDisabled("LANGUAGE ADAPTER");ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##prompt","e.g. add a capacitor",prompt_,sizeof prompt_);ImGui::BeginDisabled(busy);if(ImGui::Button("Classify part"))launch({"classify",prompt_,"--core",COPPER_CORE});ImGui::EndDisabled();ImGui::TextWrapped("The C transformer proposes a category. Project source defines values, pins and connections.");ImGui::Separator();ImGui::TextDisabled("OUTPUT DIRECTORY");ImGui::SetNextItemWidth(-1);ImGui::InputText("##out",out_,sizeof out_);ImGui::EndChild();
    ImGui::Spacing();ImGui::TextDisabled("OUTPUT / CALCULATIONS");ImGui::SameLine();ImGui::TextColored(busy?ImVec4{1,.7f,.35f,1}:ImVec4{.45f,.8f,.7f,1},"%s",status_.c_str());ImGui::BeginChild("console",{0,150},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar);ImGui::TextUnformatted(log_.c_str());ImGui::EndChild();ImGui::End();
}
