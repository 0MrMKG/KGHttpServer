#include "../include/handlers/EntryHandler.h"
#include "../include/handlers/LoginHandler.h"
#include "../include/handlers/RegisterHandler.h"
#include "../include/handlers/MenuHandler.h"
#include "../include/handlers/AiGameStartHandler.h"
#include "../include/handlers/LogoutHandler.h"
#include "../include/handlers/AiGameMoveHandler.h"
#include "../include/handlers/GameBackendHandler.h"
#include "../include/GomokuServer.h"
#include "../../../HttpServer/include/http/HttpRequest.h"
#include "../../../HttpServer/include/http/HttpResponse.h"
#include "../../../HttpServer/include/http/HttpServer.h"

using namespace http;  //HttpServer模块自定义的namespace

/* 
    @class GomokuServer
    @date 2026/1/11
    @note
        port, serverName
        GomokuServer 内部包含了一个 httpServer_ 对象（它是项目里封装的 HTTP 框架）。
        这行代码直接调用了 httpServer_ 的构造函数，把端口、名字和选项传给它。
*/
GomokuServer::GomokuServer(int port,
                           const std::string &name,
                           muduo::net::TcpServer::Option option)
    : httpServer_(port, name, option), maxOnline_(0)
{
    initialize();
}

void GomokuServer::start()
{
    httpServer_.start();
}

/* 
    --->1. threads pool set to 4
*/
void GomokuServer::setThreadNum(int numThreads)
{
    httpServer_.setThreadNum(numThreads);
}

/* 
    --->2. sql pool init
           and some session???
*/
void GomokuServer::initialize()
{

    // 初始化数据库连接池，使用正确的变量替换原来的 "root", "root"
    http::MysqlUtil::init("tcp://127.0.0.1:3306", "debian-sys-maint", "Wqnz31rhSnOSBcbc", "yourdb", 8);

    // 初始化数据库连接池
    // http::MysqlUtil::init("tcp://127.0.0.1:3306", "root", "root", "Gomoku", 10);
    // 初始化会话
    initializeSession();
    // 初始化中间件
    initializeMiddleware();
    // 初始化路由
    initializeRouter();
}


/*
    initializeSession()：初始化会话（让服务器有“记忆”）
*/
void GomokuServer::initializeSession()
{
    // 创建会话存储
    auto sessionStorage = std::make_unique<http::session::MemorySessionStorage>();
    // 创建会话管理器
    auto sessionManager = std::make_unique<http::session::SessionManager>(std::move(sessionStorage));
    // 设置会话管理器
    setSessionManager(std::move(sessionManager));
}



/*
    initializeMiddleware()：初始化中间件（让服务器守“规矩”）
    中间件就像是服务器大门前的安检员或过滤器。请求在到达你写的业务逻辑（Handler）之前，必须先经过它们。
*/
void GomokuServer::initializeMiddleware()
{
    // 创建中间件
    auto corsMiddleware = std::make_shared<http::middleware::CorsMiddleware>();
    // 添加中间件
    httpServer_.addMiddleware(corsMiddleware);
}


/* 
    initializeRouter()：初始化路由（让服务器懂“逻辑”）
    路由就像是服务器内部的分拣中心。它决定了哪个 URL 应该由哪段 C++ 代码来处理。
*/
void GomokuServer::initializeRouter()
{
    // 注册url回调处理器
    // 登录注册入口页面
    httpServer_.Get("/", std::make_shared<EntryHandler>(this));
    httpServer_.Get("/entry", std::make_shared<EntryHandler>(this));
    // 登录
    httpServer_.Post("/login", std::make_shared<LoginHandler>(this));
    // 注册
    httpServer_.Post("/register", std::make_shared<RegisterHandler>(this));
    // 登出
    httpServer_.Post("/user/logout", std::make_shared<LogoutHandler>(this));
    // 菜单页面
    httpServer_.Get("/menu", std::make_shared<MenuHandler>(this));
    // 开始对战ai
    httpServer_.Get("/aiBot/start", std::make_shared<AiGameStartHandler>(this));
    // 下棋
    httpServer_.Post("/aiBot/move", std::make_shared<AiGameMoveHandler>(this));
    // 重新开始对战ai
    httpServer_.Get("/aiBot/restart", 
    [this](const http::HttpRequest& req, http::HttpResponse* resp) {
            restartChessGameVsAi(req, resp);
    });

    // 后台界面
    httpServer_.Get("/backend", std::make_shared<GameBackendHandler>(this));
    // 后台数据获取
    httpServer_.Get("/backend_data", [this](const http::HttpRequest& req, http::HttpResponse* resp) {
        getBackendData(req, resp);
    });
}

/*
    业务逻辑处理函数，它专门负责处理用户想要“重新开始一场与 AI 的五子棋对局”的请求
*/
void GomokuServer::restartChessGameVsAi(const http::HttpRequest &req, http::HttpResponse *resp)
{
    // 解析请求体
    auto session = getSessionManager()->getSession(req, resp);
    if (session->getValue("isLoggedIn") != "true")
    {
        // 用户未登录，返回未授权错误
        json errorResp;
        errorResp["status"] = "error";
        errorResp["message"] = "Unauthorized";
        std::string errorBody = errorResp.dump(4);

        packageResp(req.getVersion(), http::HttpResponse::k401Unauthorized,
                    "Unauthorized", true, "application/json", errorBody.size(),
                    errorBody, resp);
        return;
    }

    int userId = std::stoi(session->getValue("userId"));
    {
        // 重新开始ai对战
        std::lock_guard<std::mutex> lock(mutexForAiGames_);
        if (aiGames_.find(userId) != aiGames_.end())
            aiGames_.erase(userId);
        aiGames_[userId] = std::make_shared<AiGame>(userId);
    }

    json successResp;
    successResp["status"] = "ok";
    successResp["message"] = "restart successful";
    successResp["userId"] = userId;
    std::string successBody = successResp.dump(4);
    packageResp(req.getVersion(), http::HttpResponse::k200Ok, "OK", false, "application/json", successBody.size(), successBody, resp);
}

/*
    获取后台数据
*/ 
void GomokuServer::getBackendData(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try 
    {
        // 获取数据
        int curOnline = getCurOnline();
        LOG_INFO << "当前在线人数: " << curOnline;
        
        int maxOnline = getMaxOnline();
        LOG_INFO << "历史最高在线人数: " << maxOnline;
        
        int totalUser = getUserCount();
        LOG_INFO << "已注册用户总数: " << totalUser;

        // 构造 JSON 响应
        nlohmann::json respBody;
        respBody = {
            {"curOnline", curOnline},
            {"maxOnline", maxOnline},
            {"totalUser", totalUser}
        };

        // 转换为字符串
        std::string responseStr = respBody.dump(4);
        
        // 设置响应
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setBody(responseStr);
        resp->setContentLength(responseStr.size());
        resp->setCloseConnection(false);

        LOG_INFO << "Backend data response prepared successfully";
    }
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Error in getBackendData: " << e.what();
        
        // 错误响应
        nlohmann::json errorBody = {
            {"error", "Internal Server Error"},
            {"message", e.what()}
        };
        
        std::string errorStr = errorBody.dump();
        resp->setStatusCode(http::HttpResponse::k500InternalServerError);
        resp->setStatusMessage("Internal Server Error");
        resp->setContentType("application/json");
        resp->setBody(errorStr);
        resp->setContentLength(errorStr.size());
        resp->setCloseConnection(true);
    }
}

/*
    工厂函数（Factory Function），它的核心作用是将离散的业务数据按照 HTTP 协议的规范，
    封装进一个 HttpResponse 对象中。
    在  Web 服务器中，处理完业务（如计算完牌效）后，数据只是内存里的一串字符串，浏览器无法直接理解。
    你需要通过这个函数给数据打上“物流标签”（Header），才能安全发往前端。
*/
void GomokuServer::packageResp(const std::string &version,
                             http::HttpResponse::HttpStatusCode statusCode,
                             const std::string &statusMsg,
                             bool close,
                             const std::string &contentType,
                             int contentLen,
                             const std::string &body,
                             http::HttpResponse *resp)
{
    if (resp == nullptr) 
    {
        LOG_ERROR << "Response pointer is null";
        return;
    }

    try 
    {
        resp->setVersion(version);
        resp->setStatusCode(statusCode);
        resp->setStatusMessage(statusMsg);
        resp->setCloseConnection(close);
        resp->setContentType(contentType);
        resp->setContentLength(contentLen);
        resp->setBody(body);
        
        LOG_INFO << "Response packaged successfully";
    }
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Error in packageResp: " << e.what();
        // 设置一个基本的错误响应
        resp->setStatusCode(http::HttpResponse::k500InternalServerError);
        resp->setStatusMessage("Internal Server Error");
        resp->setCloseConnection(true);
    }
}

