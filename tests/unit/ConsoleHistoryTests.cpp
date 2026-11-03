#include "ConsoleHistory.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace dk;
using namespace dk::editor::detail;

TEST_CASE("console history retains bounded nonblank attempts and adjacent deduplication") {
    ConsoleHistory history;
    CHECK_FALSE(history.remember(" \t "));
    REQUIRE(history.remember(" \tscene.query limit=1\t "));
    REQUIRE(history.remember("scene.query limit=1"));
    CHECK(history.size() == 1);
    REQUIRE(history.remember("invalid command syntax"));
    REQUIRE(history.remember("scene.query limit=1"));
    CHECK(history.size() == 3);
    for (int i = 0; i < 70; ++i) REQUIRE(history.remember("scene.query limit=" + std::to_string(i)));
    REQUIRE(history.size() == ConsoleHistory::capacity);
    CHECK(history.entries().front() == "scene.query limit=6");
    CHECK(history.entries().back() == "scene.query limit=69");
}

TEST_CASE("console history navigation restores draft and edits detach browsing") {
    ConsoleHistory history;
    CHECK(history.previous("draft") == "draft");
    CHECK(history.next("draft") == "draft");
    REQUIRE(history.remember("first")); REQUIRE(history.remember("second"));
    CHECK(history.previous("未提交 draft") == "second");
    CHECK(history.previous("second") == "first");
    CHECK(history.previous("first") == "first");
    CHECK(history.next("first") == "second");
    CHECK(history.next("second") == "未提交 draft");
    CHECK(history.next("未提交 draft") == "未提交 draft");
    CHECK(history.previous("") == "second");
    history.edited();
    CHECK(history.next("edited second") == "edited second");
    CHECK(history.previous("edited second") == "second");
    CHECK(history.next("second") == "edited second");
    REQUIRE(history.remember("third"));
    CHECK(history.next("current") == "current");
    CHECK(history.previous("") == "third");
    CHECK(history.next("third").empty());
    CHECK(history.size() == 3);
}

TEST_CASE("console history rejects invalid text without altering entries or draft") {
    ConsoleHistory history;
    REQUIRE(history.remember("first"));
    CHECK(history.previous("draft") == "first");
    for (const auto& text : {std::string(8193,'x'), std::string("scene.query\nscene.query"),
        std::string("scene.query\r"), std::string("a\0b",3), std::string("\xe4\xb8"), std::string("\xed\xa0\x80")}) {
        CHECK(console_text_problem(text).has_value());
        CHECK_FALSE(history.remember(text));
        CHECK(history.size() == 1);
    }
    CHECK(history.next("first") == "draft");
    const auto boundary = std::string(8189,'x') + "中";
    CHECK_FALSE(console_text_problem(boundary));
    REQUIRE(history.remember(boundary));
    CHECK(history.entries().back() == boundary);
    CHECK_FALSE(console_text_problem("a\tb 中文 😀"));
}

TEST_CASE("console logs stay bounded at complete UTF8 codepoints") {
    CHECK(console_log_text("plain 中文 😀") == "plain 中文 😀");
    CHECK(console_log_text(std::string(4096,'x')).size() == 4096);
    for (const std::string unit : {"中", "😀"}) {
        for (int offset = 0; offset < 4; ++offset) {
            std::string message(offset,'x');
            for (int i = 0; i < 5000; ++i) message += unit;
            const auto result = console_log_text(message);
            CHECK(result.size() <= 4096);
            CHECK(result.ends_with(" ... (truncated; use IPC for full result)"));
            CHECK(validate_command_value(Json(result),1024*1024));
        }
    }
}
