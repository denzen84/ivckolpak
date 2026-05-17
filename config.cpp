#include "config.h"
#include "config_repository.hpp"
#include "logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <stdexcept>
#include <cstdint>
#include <cstdio>

static std::string trim(const std::string& s) {
    auto a=s.find_first_not_of(" \t\r\n");
    if(a==std::string::npos) return "";
    auto b=s.find_last_not_of(" \t\r\n");
    return s.substr(a, b-a+1);
}

static bool iequals(const std::string& a, const std::string& b) {
    return std::equal(a.begin(),a.end(),b.begin(),b.end(),[](char c1, char c2){return std::tolower(c1)==std::tolower(c2);});
}

static std::shared_ptr<CameraConfig> merge_configs(const GlobalConfig& global, const CameraConfig& cam) {
    auto result = std::make_shared<CameraConfig>(cam);
#define MERGE_STR(field) if (result->field.empty()) result->field = global.field;
#define MERGE_INT(field) if (result->field == 0) result->field = global.field;
#define MERGE_DBL(field) if (result->field == 0.0) result->field = global.field;
    MERGE_STR(target_dir); MERGE_STR(filename_format);
    MERGE_STR(on_event_start); MERGE_STR(on_event_stop); MERGE_STR(on_video_save);
    MERGE_STR(on_rtsp_lost); MERGE_STR(on_rtsp_found);
    MERGE_INT(pre_buffer_iframes); MERGE_INT(post_buffer_iframes);
    MERGE_DBL(max_chunk_duration_time_s); MERGE_DBL(max_event_total_duration_s);
    MERGE_INT(max_event_chunks); MERGE_INT(max_chunk_kbytes);
    MERGE_INT(reconnect_delay_ms); MERGE_INT(reconnect_max_delay_ms);
    return result;
}

std::string extract_ip_hex_from_rtsp(const std::string& url) {
    size_t p=url.find("://");
    if(p==std::string::npos) return "0x00000000";
    size_t s=p+3, at=url.find('@',s);
    if(at!=std::string::npos) s=at+1;
    size_t e=url.find('/',s);
    if(e==std::string::npos) e=url.find(':',s);
    if(e==std::string::npos) e=url.length();
    std::string h=url.substr(s,e-s);
    size_t pp=h.find(':');
    if(pp!=std::string::npos) h=h.substr(0,pp);
    uint32_t ip=0; int a,b,c,d;
    if(std::sscanf(h.c_str(),"%d.%d.%d.%d",&a,&b,&c,&d)==4)
        ip=(a<<24)|(b<<16)|(c<<8)|d;
    std::ostringstream o;
    o<<"0x"<<std::hex<<std::uppercase<<std::setw(8)<<std::setfill('0')<<ip;
    return o.str();
}

std::string sanitize_filename(std::string n) {
    std::string inv="<>:\"/\\|?*";
    for(char& c:n) if(inv.find(c)!=std::string::npos || (unsigned char)c<32) c='_';
    if(n.length()>255) n=n.substr(0,255);
    return n;
}

std::string format_filename(const std::string& fmt, const CameraConfig& cfg, int chunk_idx, const AlarmEvent* evt) {
    std::ostringstream result;
    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    char time_buf[32];
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%') {
            if (i + 1 < fmt.size()) {
                char next = fmt[i + 1];
                switch (next) {
                    case 'Y': std::strftime(time_buf, sizeof(time_buf), "%Y", &tm); result << time_buf; i++; continue;
                    case 'm': std::strftime(time_buf, sizeof(time_buf), "%m", &tm); result << time_buf; i++; continue;
                    case 'd': std::strftime(time_buf, sizeof(time_buf), "%d", &tm); result << time_buf; i++; continue;
                    case 'H': std::strftime(time_buf, sizeof(time_buf), "%H", &tm); result << time_buf; i++; continue;
                    case 'M': std::strftime(time_buf, sizeof(time_buf), "%M", &tm); result << time_buf; i++; continue;
                    case 'S': std::strftime(time_buf, sizeof(time_buf), "%S", &tm); result << time_buf; i++; continue;
                    case 'T': std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm); result << time_buf; i++; continue;
                    case 'v': result << chunk_idx; i++; continue;
                    case 'e': result << (evt && evt->event_id > 0 ? evt->event_id : 0); i++; continue;
                }
                if (next == '{') {
                    size_t end = fmt.find('}', i + 2);
                    if (end != std::string::npos) {
                        std::string key = fmt.substr(i + 2, end - i - 2);
                        if (key == "cam_id") result << cfg.serialid;
                        else if (key == "cam_name") result << cfg.name;
                        else if (key == "cam_ip") result << extract_ip_hex_from_rtsp(cfg.rtsp_url);
                        else if (evt) {
                            if (key == "json_addr") result << evt->address;
                            else if (key == "json_chan") result << evt->channel;
                            else if (key == "json_desc") result << evt->description;
                            else if (key == "json_event") result << evt->event_type;
                            else if (key == "json_serialid") result << evt->serial_id;
                            else if (key == "json_starttime") result << evt->start_time;
                            else if (key == "json_status") result << evt->status;
                            else if (key == "json_alarm") result << evt->alarm_type;
                        }
                        i = end;
                        continue;
                    }
                }
            }
        }
        result << fmt[i];
    }
    return sanitize_filename(result.str());
}

ParsedConfig load_config(const std::string& path) {
    ParsedConfig res;
    std::ifstream f(path);
    if(!f) throw std::runtime_error("Cannot open config");
    CameraConfig cur;
    std::string sec, line;
    auto global = std::make_shared<GlobalConfig>();
    while(std::getline(f,line)){
        line=trim(line);
        if(line.empty()||line[0]=='#'||line[0]==';') continue;
        if(line[0]=='['){
            if(sec=="camera"&&!cur.serialid.empty()) {
                auto cfg = merge_configs(*global, cur);
                res.cameras.push_back(cfg);
                ConfigRepository::instance().register_camera(cfg->serialid, cfg);
            }
            sec=line.substr(1,line.size()-2);
            cur=CameraConfig{};
            continue;
        }
        auto eq=line.find('=');
        if(eq==std::string::npos) continue;
        std::string k=trim(line.substr(0,eq)), v=trim(line.substr(eq+1));
        auto set_g=[&](const std::string& key, const std::string& val){
            if(key=="pre_buffer_iframes") global->pre_buffer_iframes=std::stoi(val);
            else if(key=="post_buffer_iframes") global->post_buffer_iframes=std::stoi(val);
            else if(key=="max_chunk_duration_time_s") global->max_chunk_duration_time_s=std::stod(val);
            else if(key=="max_event_total_duration_s") global->max_event_total_duration_s=std::stod(val);
            else if(key=="max_event_chunks") global->max_event_chunks=std::stoi(val);
            else if(key=="max_chunk_kbytes") global->max_chunk_kbytes=std::stoi(val);
            else if(key=="target_dir") global->target_dir=val;
            else if(key=="filename_format") global->filename_format=val;
            else if(key=="on_event_start") global->on_event_start=val;
            else if(key=="on_event_stop") global->on_event_stop=val;
            else if(key=="on_video_save") global->on_video_save=val;
            else if(key=="on_rtsp_lost") global->on_rtsp_lost=val;
            else if(key=="on_rtsp_found") global->on_rtsp_found=val;
            else if(key=="reconnect_delay_ms") global->reconnect_delay_ms=std::stoi(val);
            else if(key=="reconnect_max_delay_ms") global->reconnect_max_delay_ms=std::stoi(val);
            else if(key=="disable_logs") {
                global->disable_logs = (val=="yes" || val=="true" || val=="1" || iequals(val,"on"));
            }
        };
        auto set_c=[&](const std::string& key, const std::string& val){
            if(key=="name") cur.name=val;
            else if(key=="description") cur.description=val;
            else if(key=="serialid") cur.serialid=val;
            else if(key=="rtsp") cur.rtsp_url=val;
            else if(key=="max_chunk_duration_time_s") cur.max_chunk_duration_time_s=std::stod(val);
            else if(key=="max_event_total_duration_s") cur.max_event_total_duration_s=std::stod(val);
            else if(key=="max_event_chunks") cur.max_event_chunks=std::stoi(val);
            else if(key=="max_chunk_kbytes") cur.max_chunk_kbytes=std::stoi(val);
            else if(key=="pre_buffer_iframes") cur.pre_buffer_iframes=std::stoi(val);
            else if(key=="post_buffer_iframes") cur.post_buffer_iframes=std::stoi(val);
            else if(key=="target_dir") cur.target_dir=val;
            else if(key=="filename_format") cur.filename_format=val;
            else if(key=="on_event_start") cur.on_event_start=val;
            else if(key=="on_event_stop") cur.on_event_stop=val;
            else if(key=="on_video_save") cur.on_video_save=val;
            else if(key=="on_rtsp_lost") cur.on_rtsp_lost=val;
            else if(key=="on_rtsp_found") cur.on_rtsp_found=val;
            else if(key=="reconnect_delay_ms") cur.reconnect_delay_ms=std::stoi(val);
            else if(key=="reconnect_max_delay_ms") cur.reconnect_max_delay_ms=std::stoi(val);
            else if(key=="rtsp_over_udp") cur.rtsp_over_udp=(val=="on"||val=="1"||iequals(val,"true"));
        };
        if(sec=="global") set_g(k,v);
        else if(sec=="camera") set_c(k,v);
        else if(sec=="alarm_server"&&k=="listen_port") global->alarm_server_port=std::stoi(v);
    }
    if(sec=="camera"&&!cur.serialid.empty()) {
        auto cfg = merge_configs(*global, cur);
        res.cameras.push_back(cfg);
        ConfigRepository::instance().register_camera(cfg->serialid, cfg);
    }
    for (const auto& cam : res.cameras) {
        if (cam->serialid.empty()) {
            throw std::runtime_error("Camera configuration error: 'serialid' is required but missing");
        }
        if (cam->rtsp_url.empty()) {
            throw std::runtime_error("Camera '" + cam->serialid + "': 'rtsp' URL is required but missing");
        }
    }
    for (const auto& cam : res.cameras) {
        LOG_INFO("CONFIG", "Loaded camera: name='", cam->name, "' serialid='", cam->serialid, "'");
    }
    res.global = global;
    ConfigRepository::instance().set_global(global);
    return res;
}