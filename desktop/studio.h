#pragma once
#include "imgui.h"
#include <string>
#include <future>
#include <memory>
#include <vector>
extern "C" {
#include "v2/project.h"
}
class Studio {
public:
 explicit Studio(const char *path);
 ~Studio();
 void theme();
 void draw();
private:
 char path_[2048]{},out_[2048]{},prompt_[512]{},filter_[128]{};
 std::vector<char> editor_;
 std::string log_,status_="Ready",parse_error_;
 std::future<std::string> job_;
 CpProject *project_=nullptr;
 int selected_=-1;float zoom_=1;ImVec2 pan_{0,0};
 bool dirty_=false,show_source_=false,top_=true,bottom_=true;
 void load_file(const char *path);
 bool save_file();
 void refresh(const char *path);
 void launch(std::vector<std::string> args);
 void board();
};
