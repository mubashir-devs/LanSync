#include "storage/database/sql.hpp"

namespace lansync::storage {

Result<void> bindAll(Statement& st, SqlParams params) {
    int index = 1;
    for (const auto& p : params) {
        Result<void> r = std::visit(
            [&](const auto& x) -> Result<void> {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) return st.bindNull(index);
                else if constexpr (std::is_same_v<T, std::int64_t>) return st.bindInt(index, x);
                else if constexpr (std::is_same_v<T, double>) return st.bindReal(index, x);
                else return st.bindText(index, x);
            },
            p.v);
        if (!r) return r;
        ++index;
    }
    return {};
}

Result<void> run(Database& db, std::string_view sql, SqlParams params) {
    auto st = db.prepare(sql);
    if (!st) return st.error();
    if (auto r = bindAll(*st, params); !r) return r;
    auto step = st->step();
    if (!step) return step.error();
    return {};
}

Result<void> query(Database& db, std::string_view sql, SqlParams params,
                   const std::function<void(Statement&)>& onRow) {
    auto st = db.prepare(sql);
    if (!st) return st.error();
    if (auto r = bindAll(*st, params); !r) return r;
    for (;;) {
        auto step = st->step();
        if (!step) return step.error();
        if (!*step) return {};
        onRow(*st);
    }
}

std::optional<std::string> optText(const Statement& st, int col) {
    if (st.columnIsNull(col)) return std::nullopt;
    return st.columnText(col);
}
std::optional<std::int64_t> optInt(const Statement& st, int col) {
    if (st.columnIsNull(col)) return std::nullopt;
    return st.columnInt(col);
}

}  // namespace lansync::storage
