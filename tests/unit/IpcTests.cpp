#include "SceneTestFiles.hpp"
#include <dk/automation/Client.hpp>
#include <dk/automation/IpcServer.hpp>
#include <future>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

using namespace dk;
using namespace std::chrono_literals;
namespace
{
std::string endpoint() { return "test-" + TaskId::generate().value().to_string(); }
ipc::Deadline deadline() { return std::chrono::steady_clock::now() + 5s; }
struct Fixture
{
    SceneTestFiles files;
    std::unique_ptr<Runtime> runtime = Runtime::create(files.root).value();
};
Json reserve(IpcSession& session)
{
    return Json::parse(session.handle(R"({"protocol":1,"hello":true,"reserve":true})"));
}
Json frame(const Json& hello, std::string method, Json params = Json::object())
{
    return {{"protocol",1},{"session",hello.at("session")},{"request",{
        {"jsonrpc","2.0"},{"id",hello.at("request_id")},{"method",std::move(method)},{"params",std::move(params)}}}};
}
Json handle(IpcSession& session, const Json& request) { return Json::parse(session.handle(request.dump())).at("response"); }
Json guard(const Json& state) { return {{"document_id",state.at("document_id")},{"revision",state.at("revision")}}; }
template<class T, class Pump> T complete(std::future<T>& future, Pump pump)
{
    const auto until = deadline();
    while (future.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() < until)
    {
        pump();
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(future.wait_for(0ms) == std::future_status::ready);
    return future.get();
}
ipc::Incoming incoming(ipc::PipeServer& server)
{
    const auto until = deadline();
    while (std::chrono::steady_clock::now() < until)
    {
        if (auto value = server.pop()) return std::move(*value);
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error{"No incoming frame"};
}
struct NativePeer
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit NativePeer(const std::string& name)
    {
        const std::wstring path = L"\\\\.\\pipe\\DeckerEngine." + std::wstring{name.begin(),name.end()};
        handle = CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error{"raw client connect failed"};
    }
    ~NativePeer() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    void write(std::string_view bytes)
    {
        DWORD done{};
        if (!WriteFile(handle,bytes.data(),static_cast<DWORD>(bytes.size()),&done,nullptr) || done != bytes.size())
            throw std::runtime_error{"raw client write failed"};
    }
};
}
TEST_CASE("IPC tickets replay the same TaskId and never repeat a mutation")
{
    Fixture f;
    IpcSession session{*f.runtime};
    const auto start = handle(session,frame(reserve(session),"scene.new"));
    const auto ticket = reserve(session);
    auto request = frame(ticket,"entity.create",{{"guard",guard(start["result"]["value"])}});
    const auto first = handle(session,request);
    REQUIRE(first.contains("result"));
    CHECK(handle(session,request) == first);
    request["request"]["params"]["id"] = EntityId::generate()->to_string();
    CHECK(handle(session,request)["error"]["code"] == -32072);
    const auto queried = handle(session,frame(reserve(session),"scene.query"));
    CHECK(queried["result"]["value"]["state"]["entity_count"] == 1);
    auto history = handle(session,frame(reserve(session),"history.status"));
    CHECK(history["result"]["value"]["undo_count"] == 1);
}
TEST_CASE("IPC expired unissued and restarted tickets fail closed")
{
    Fixture f;
    IpcSession session{*f.runtime,{2}};
    const auto hello = reserve(session);
    const auto request = frame(hello,"scene.new");
    REQUIRE(handle(session,request).contains("result"));
    (void)reserve(session); (void)reserve(session);
    CHECK(handle(session,request)["error"]["code"] == -32071);
    auto invented = frame(reserve(session),"scene.new");
    invented["request"]["id"] = 99999;
    CHECK(handle(session,invented)["error"]["code"] == -32071);
    IpcSession restarted{*f.runtime};
    CHECK(handle(restarted,request)["error"]["code"] == -32073);
    auto off_thread = std::async(std::launch::async,[&] {
        try { (void)session.handle("{}"); return false; } catch (const std::logic_error&) { return true; }
    });
    CHECK(off_thread.get());
}
TEST_CASE("IPC invalid envelopes cannot allocate Runtime tasks")
{
    Fixture f; IpcSession session{*f.runtime};
    for (const auto* text : {"[]","{}","{",R"({"protocol":2})",R"({"protocol":1,"protocol":1})",
            R"({"protocol":1,"hello":true,"reserve":true,"extra":0})"})
        CHECK(Json::parse(session.handle(text))["response"].contains("error"));
    auto request = frame(reserve(session),"scene.new");
    request["request"].erase("id");
    CHECK(handle(session,request)["error"]["code"] == -32070);
    auto tasks = f.runtime->dispatch("tasks.list",Json::object());
    REQUIRE(tasks); REQUIRE(tasks->result);
    CHECK(tasks->result->empty());
}
TEST_CASE("IPC ledger byte budget evicts old payloads without making them executable")
{
    Fixture f; IpcSession session{*f.runtime,{256,ipc::max_request_bytes+ipc::max_response_bytes}};
    const auto request = frame(reserve(session),"unknown",{{"payload",std::string(750000,'x')}});
    CHECK(handle(session,request)["error"]["code"] == -32601);
    CHECK(handle(session,frame(reserve(session),"unknown",{{"payload",std::string(750000,'y')}}))["error"]["code"] == -32601);
    CHECK(handle(session,request)["error"]["code"] == -32071);
    CHECK(f.runtime->dispatch("tasks.list",Json::object())->result->empty());
}
TEST_CASE("IPC client reads large replies and classifies malformed replies as unknown")
{
    for (const auto kind : {0,1,2,3})
    {
        const auto name = endpoint(); const auto session = TaskId::generate()->to_string();
        auto server = ipc::PipeServer::listen(name,[] {}).value();
        auto future = std::async(std::launch::async,[&] { return ipc::call({name},"commands.list"); });
        auto hello = incoming(*server);
        hello.reply(Json{{"protocol",1},{"session",session},{"request_id",1},{"limits",Json::object()}}.dump());
        auto command = incoming(*server);
        Json response{{"jsonrpc","2.0"},{"id",1},{"result",std::string(2*1024*1024,'x')}};
        if (kind == 1) response["id"] = 2;
        if (kind == 2) response["id"] = 1.0;
        if (kind == 3) { response.erase("result"); response["error"] = "invalid error"; }
        command.reply(Json{{"protocol",1},{"session",session},{"response",response}}.dump());
        const auto result = future.get();
        if (kind == 0) { CHECK(result.exit_code() == 0); CHECK(result.execution == "received"); }
        else { CHECK(result.status == "invalid"); CHECK(result.execution == "unknown"); REQUIRE(result.ticket); }
    }
}
TEST_CASE("Named pipe dispatch stays on owner and shutdown reply reaches client")
{
    Fixture f; const auto name = endpoint();
    auto server = IpcServer::listen(*f.runtime,name).value();
    CHECK_FALSE(IpcServer::listen(*f.runtime,name));
    auto future = std::async(std::launch::async,[&] { return ipc::call({name},"scene.new"); });
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(f.runtime->dispatch("scene.query",Json::object())->result);
    auto created = complete(future,[&] { (void)server->pump(); });
    REQUIRE(created.exit_code() == 0);
    REQUIRE(created.ticket);
    CHECK(created.execution == "received");
    auto replay = std::async(std::launch::async,[&] { return ipc::call({name,5s,created.ticket},"scene.new"); });
    CHECK(complete(replay,[&] { (void)server->pump(); }).response == created.response);
    auto stop = std::async(std::launch::async,[&] { return ipc::call({name},"runtime.shutdown"); });
    const auto until = deadline();
    while (!f.runtime->stopping() && std::chrono::steady_clock::now() < until) { (void)server->pump(); std::this_thread::sleep_for(1ms); }
    REQUIRE(f.runtime->stopping());
    server->close(1500ms);
    CHECK(stop.get().exit_code() == 0);
}
TEST_CASE("Disconnected accepted request executes once and explicit retry recovers result")
{
    Fixture f; const auto name = endpoint(); IpcSession session{*f.runtime};
    auto server = ipc::PipeServer::listen(name,[] {},{8,200ms}).value();
    auto future = std::async(std::launch::async,[&] { return ipc::call({name,100ms},"scene.new"); });
    auto hello = incoming(*server); hello.reply(session.handle(hello.bytes()));
    auto command = incoming(*server);
    // The entire request was accepted, but the owner has not dispatched it yet.
    const auto timed_out = future.get();
    CHECK(timed_out.status == "timeout"); CHECK(timed_out.execution == "unknown"); REQUIRE(timed_out.ticket);
    CHECK_FALSE(f.runtime->dispatch("scene.query",Json::object())->result);
    const auto original = Json::parse(session.handle(command.bytes())).at("response");
    command.reply(Json{{"protocol",1},{"session",session.id()},{"response",original}}.dump());
    auto retry = std::async(std::launch::async,[&] { return ipc::call({name,5s,timed_out.ticket},"scene.new"); });
    const auto recovered = complete(retry,[&] { if (auto item = server->pop()) item->reply(session.handle(item->bytes())); });
    CHECK(recovered.exit_code() == 0); CHECK(recovered.response == original);
}
TEST_CASE("Pipe framing accepts fragmented headers and rejects oversized or truncated bodies")
{
    SECTION("fragmented frame")
    {
        const auto name = endpoint(); auto server = ipc::PipeServer::listen(name,[] {}).value();
        NativePeer peer{name};
        for (char c : std::string{"\x02\0\0\0{}",6}) peer.write(std::string_view{&c,1});
        auto frame = incoming(*server); CHECK(frame.bytes() == "{}"); frame.reply("{}");
        std::array<char,6> output{}; DWORD bytes{};
        REQUIRE(ReadFile(peer.handle,output.data(),6,&bytes,nullptr));
        CHECK(bytes >= 4); CHECK(output[0] == 2);
    }
    SECTION("oversized length")
    {
        const auto name = endpoint(); auto server = ipc::PipeServer::listen(name,[] {},{8,60ms}).value();
        NativePeer peer{name}; peer.write(std::string{"\x01\0\x10\0",4});
        char byte{}; DWORD count{};
        CHECK_FALSE(ReadFile(peer.handle,&byte,1,&count,nullptr)); CHECK_FALSE(server->pop());
    }
    SECTION("truncated body")
    {
        const auto name = endpoint(); auto server = ipc::PipeServer::listen(name,[] {},{8,60ms}).value();
        { NativePeer peer{name}; peer.write(std::string{"\x20\0\0\0x",5}); }
        std::this_thread::sleep_for(80ms); CHECK_FALSE(server->pop());
    }
}
TEST_CASE("Pipe queues are bounded and idle shutdown cancels IO")
{
    const auto name = endpoint(); auto server = ipc::PipeServer::listen(name,[] {},{1,40ms}).value();
    for (int i=0;i<2;++i)
    {
        auto client = ipc::Connection::connect(name,deadline()).value();
        REQUIRE(client.send(i == 0 ? "first" : "second",deadline()));
        CHECK_FALSE(client.receive(deadline()));
    }
    auto first = server->pop(); REQUIRE(first); CHECK(first->bytes() == "first"); CHECK_FALSE(server->pop());
    const auto start = std::chrono::steady_clock::now();
    server->close(); CHECK(std::chrono::steady_clock::now()-start < 1s);
    auto unavailable = ipc::call({endpoint(),30ms},"scene.new");
    CHECK(unavailable.status == "timeout"); CHECK(unavailable.execution == "not_sent"); CHECK_FALSE(unavailable.ticket);
    CHECK_FALSE(ipc::valid_endpoint("../remote")); CHECK_FALSE(ipc::valid_endpoint(""));
}
TEST_CASE("Pipe grace close does not wait for a new connection")
{
    const auto name=endpoint();
    auto server=ipc::PipeServer::listen(name,[] {}).value();
    SECTION("unused listener") {}
    SECTION("peer has received its reply and disconnected") {
        auto peer=std::async(std::launch::async,[&] {
            auto connection=ipc::Connection::connect(name,deadline()).value();
            if (!connection.send("request",deadline())) throw std::runtime_error{"send failed"};
            return connection.receive(deadline());
        });
        auto frame=incoming(*server); frame.reply("reply");
        const auto reply=peer.get(); REQUIRE(reply); CHECK(*reply=="reply");
    }
    // Let the IO thread enter/re-enter ConnectNamedPipe before requesting grace.
    std::this_thread::sleep_for(30ms);
    const auto start=std::chrono::steady_clock::now();
    server->close(1500ms);
    CHECK(std::chrono::steady_clock::now()-start < 1s);
}
