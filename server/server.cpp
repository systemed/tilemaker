#include "Simple-Web-Server/server_http.hpp"
#include "external/sqlite_modern_cpp.h"
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

using namespace std;
namespace po = boost::program_options;
using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

#include <iostream>
#include <fstream>

inline unsigned char from_hex (unsigned char ch) {
    if (ch <= '9' && ch >= '0')
        ch -= '0';
    else if (ch <= 'f' && ch >= 'a')
        ch -= 'a' - 10;
    else if (ch <= 'F' && ch >= 'A')
        ch -= 'A' - 10;
    else
        ch = 0;
    return ch;
}

const std::string urldecode (const std::string& str) {
    string result;
    string::size_type i;
    for (i = 0; i < str.size(); ++i)
    {
        if (str[i] == '+')
        {
            result += ' ';
        }
        else if (str[i] == '%' && str.size() > i+2)
        {
            const unsigned char ch1 = from_hex(str[i+1]);
            const unsigned char ch2 = from_hex(str[i+2]);
            const unsigned char ch = (ch1 << 4) | ch2;
            result += ch;
            i += 2;
        }
        else
        {
            result += str[i];
        }
    }
    return result;
}

int main(int argc, char* argv[]) {
    string input;
    string staticPath;
    unsigned port;
    po::options_description desc("tilemaker-server\nServe tiles from an .mbtiles archive\n\nAvailable options");
    desc.add_options()
        ("help","show help message")
        ("input",po::value< string >(&input),"source .mbtiles")
        ("static", po::value< string >(&staticPath)->default_value("static"), "path of static files")
        ("port",po::value< unsigned >(&port)->default_value(8080), "port to serve tiles");
    po::positional_options_description p;
    p.add("input", -1);
    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    } catch (const po::unknown_option& ex) {
        cerr << "Unknown option: " << ex.get_option_name() << endl;
        return -1;
    }
    po::notify(vm);
    if (vm.count("help")) { std::cout << desc << std::endl; return 1; }
    if (vm.count("input") == 0) { std::cerr << "You must specify an .mbtiles file. Run with --help to find out more." << std::endl; return -1; }

    HttpServer server;
    server.config.port = port;
    sqlite::database db;
    db.init(input);

    server.resource["^/([0-9]+)/([0-9]+)/([0-9]+).pbf$"]["GET"] = [&db](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
        int32_t zoom = stoi(request->path_match[1]);
        int32_t col = stoi(request->path_match[2]);
        int32_t y = stoi(request->path_match[3]);
        vector<char> pbfBlob;
        int tmsY = pow(2,zoom) - 1 - y;
        db << "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?" << zoom << col << tmsY >> pbfBlob;
        std::string outStr(pbfBlob.begin(), pbfBlob.end());
        SimpleWeb::CaseInsensitiveMultimap header;
        header.emplace("Content-Encoding", "gzip");
        header.emplace("Access-Control-Allow-Origin", "*");
        response->write(outStr,header);
    };

    server.resource["^/metadata$"]["GET"] = [&db](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
       rapidjson::Document document;
       document.SetObject();
       rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
        db << "SELECT name, value FROM metadata;" >> [&](string name, string value) {
            if (name == "json") {
                rapidjson::Document subDocument;
                subDocument.Parse(value.c_str());
                document.AddMember("json",subDocument,allocator);

            } else {
                rapidjson::Value nameVal;
                nameVal.SetString(name.c_str(), allocator);
                rapidjson::Value valueVal;
                valueVal.SetString(value.c_str(), allocator);
                document.AddMember(nameVal, valueVal, allocator);
            }
        };
       rapidjson::StringBuffer stringbuf;
       rapidjson::Writer<rapidjson::StringBuffer> writer(stringbuf);
       document.Accept(writer);
       response->write(stringbuf.GetString());
    };

    server.default_resource["GET"] = [&staticPath](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
        try {
            auto pathstr = urldecode(request->path);
            if (pathstr == "/") pathstr = "/index.html";
            auto web_root_path = boost::filesystem::canonical(staticPath);
            auto path = boost::filesystem::canonical(web_root_path / pathstr);
            if(distance(web_root_path.begin(), web_root_path.end()) > distance(path.begin(), path.end()) ||
                !equal(web_root_path.begin(), web_root_path.end(), path.begin()))
                throw invalid_argument("path must be within root path");

            SimpleWeb::CaseInsensitiveMultimap header;
            auto ifs = make_shared<ifstream>();
            ifs->open(path.string(), ifstream::in | ios::binary | ios::ate);
            if(*ifs) {
                auto length = ifs->tellg();
                ifs->seekg(0, ios::beg);
                header.emplace("Content-Length", to_string(length));
                response->write(header);
                vector<char> buffer(length);
                ifs->read(&buffer[0],length);
                response->write(&buffer[0],length);
            } else {
                throw invalid_argument("could not read file");
            }
        } catch(const exception &e) {
            response->write(SimpleWeb::StatusCode::client_error_bad_request, "Could not open path " + request->path + ": " + e.what());
        }
    };

    cout << "Starting local server on port " << server.config.port << endl;
    server.start();
}