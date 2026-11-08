// The reverse-engineering commands of the agent protocol: analyze, routines, references, listing,
// jump_tables, data_regions and the annotations (names and comments). See docs/reverse-engineering.md.

#include <algorithm>
#include <stdexcept>

#include "m6510/c64.hpp"
#include "m6510/disassembler.hpp"
#include "m6510/format.hpp"
#include "m6510/image_io.hpp"
#include "m6510/kernal.hpp"
#include "m6510/listing.hpp"
#include "m6510/petscii.hpp"
#include "m6510/session.hpp"

namespace m6510 {

namespace {

struct AnalysisError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string hex4(unsigned v) { return "0x" + hexDigits(v & 0xFFFF, 4); }
std::string hex2(unsigned v) { return "0x" + hexDigits(v & 0xFF, 2); }

const char* kindName(JumpTable::Kind k) {
    switch (k) {
        case JumpTable::Kind::Vector: return "vector";
        case JumpTable::Kind::Rts: return "rts";
        case JumpTable::Kind::Lohi: return "lohi";
        case JumpTable::Kind::Pointers: return "pointers";
    }
    return "?";
}

}  // namespace

bool Session::isAnalysisCommand(const std::string& cmd) {
    static const char* names[] = {"analyze", "jump_tables", "data_regions", "references", "routines", "listing", "ann_label",
                                  "ann_routine", "ann_variable", "ann_data", "ann_comment", "ann_constant", "ann_remove",
                                  "ann_title", "ann_list", "ann_save", "ann_load"};
    for (const char* n : names)
        if (cmd == n) return true;
    return false;
}

Json Session::analysisCommand(const std::string& cmd, const Json& req) {
    Annotations& ann = ann_;
    auto arg = [&](const char* key) -> const Json* {
        const Json* v = req.find(key);
        return v && !v->isNull() ? v : nullptr;
    };
    auto str = [&](const char* key) -> std::string {
        const Json* v = arg(key);
        if (!v || !v->isString()) throw AnalysisError(std::string("missing string '") + key + "'");
        return v->asString();
    };
    auto strOpt = [&](const char* key) -> std::string {
        const Json* v = arg(key);
        return v && v->isString() ? v->asString() : std::string();
    };
    auto flag = [&](const char* key, bool def = false) {
        const Json* v = arg(key);
        if (!v) return def;
        if (!v->isBool()) throw AnalysisError(std::string("'") + key + "' must be true or false");
        return v->asBool();
    };
    auto addressOf = [&](const Json& v, const char* what) { return static_cast<Address>(argValue(v, what) & 0xFFFF); };
    auto address = [&](const char* key) {
        const Json* v = arg(key);
        if (!v) throw AnalysisError(std::string("missing '") + key + "'");
        return addressOf(*v, key);
    };
    auto endAddress = [&](const char* key) -> std::uint32_t {  // an end may be $10000
        const Json* v = arg(key);
        if (!v) throw AnalysisError(std::string("missing '") + key + "'");
        const std::int64_t e = argValue(*v, key);
        if (e < 1 || e > 0x10000) throw AnalysisError(std::string(key) + " must be 1..0x10000");
        return static_cast<std::uint32_t>(e);
    };
    auto number = [&](const char* key, std::int64_t def, std::int64_t lo, std::int64_t hi) {
        const Json* v = arg(key);
        if (!v) return def;
        const std::int64_t n = argValue(*v, key);
        if (n < lo || n > hi) throw AnalysisError(std::string(key) + " must be " + std::to_string(lo) + ".." + std::to_string(hi));
        return n;
    };
    auto failIf = [](const std::optional<std::string>& err) {
        if (err) throw AnalysisError(*err);
    };
    auto requireFiles = [&]() {
        if (!options_.allowFiles) throw AnalysisError("file access is disabled (start the server with --allow-files)");
    };
    Json ok = Json::object().set("ok", true);

    // ---- analysis ----
    if (cmd == "analyze") {
        if (flag("reset")) {
            analysisEntries_.clear();
            linearRanges_.clear();
        }
        Address from = 0;
        std::uint32_t to = 0;
        if (arg("from") || arg("to")) {
            from = address("from");
            to = endAddress("to");
        } else if (m_.programRange) {
            from = m_.programRange->first;
            to = m_.programRange->second;
        } else {
            throw AnalysisError("nothing was loaded or assembled yet: give 'from' and 'to'");
        }
        if (to <= from) throw AnalysisError("'to' must be above 'from'");
        if (const Json* e = arg("entries")) {
            if (!e->isArray()) throw AnalysisError("'entries' must be an array");
            for (const Json& x : e->items()) analysisEntries_.insert(addressOf(x, "entries"));
        }
        if (const Json* l = arg("linear")) {
            if (!l->isObject() || !l->find("from") || !l->find("to")) throw AnalysisError("linear needs {\"from\":…, \"to\":…}");
            const std::int64_t lt = argValue(*l->find("to"), "linear.to");
            if (lt < 1 || lt > 0x10000) throw AnalysisError("linear.to must be 1..0x10000");
            linearRanges_.emplace_back(addressOf(*l->find("from"), "linear.from"), static_cast<std::uint32_t>(lt));
        }
        std::set<Address> entries = analysisEntries_;
        Json defaults = Json::array();
        if (entries.empty() && linearRanges_.empty()) {
            // default: the SYS address of a BASIC starter at the start, else the PC (if it is inside the range)
            Address e = m_.cpu().registers().pc;
            if (from == 0x0801) {
                if (const auto t = findSysTarget(m_.memory(), 0x0801)) e = *t;
            }
            if (e < from || e >= to) e = from;
            entries.insert(e);
            defaults.push(hex4(e));
        }
        analysisView_ = std::make_unique<Memory>(m_.memory());
        analysis_ = std::make_unique<Analysis>(*analysisView_, from, to);
        analysis_->setDataBarrier([this](Address a) { return ann_.dataCovering(a) != nullptr; });
        for (const Address e : entries) analysis_->addEntry(e);
        for (const auto& [ra, region] : ann.dataRegions()) {
            if (region.kind == DataKind::Pointers) analysis_->addPointerTable(ra, region.length, false);
            if (region.kind == DataKind::RtsPointers) analysis_->addPointerTable(ra, region.length, true);
            if (region.kind == DataKind::Lohi) analysis_->addLohiTable(ra, region.partner, region.length, region.minusOne);
        }
        analysis_->analyzeFlow();
        for (const auto& [lf, lt] : linearRanges_) analysis_->analyzeLinear(lf, lt);
        Json j = ok;
        j.set("from", hex4(from)).set("to", to == 0x10000 ? std::string("0x10000") : hex4(to));
        j.set("code_bytes", analysis_->codeBytes()).set("range_bytes", to - from);
        j.set("instructions", analysis_->code().size());
        j.set("routines", analysis_->routines().size());
        j.set("jump_tables", analysis_->jumpTables().size());
        Json ej = Json::array();
        for (const Address e : entries) ej.push(hex4(e));
        j.set("entries", ej);
        if (!defaults.items().empty()) j.set("default_entry", true);
        Json vec = Json::array();
        for (const auto& [h, v] : analysis_->vectorEntries()) vec.push(Json::object().set("handler", hex4(h)).set("vector", hex4(v)));
        j.set("interrupt_handlers", vec);
        return j;
    }

    auto needAnalysis = [&]() -> Analysis& {
        if (!analysis_) throw AnalysisError("no analysis yet: use 'analyze' first");
        return *analysis_;
    };
    auto routineName = [&](Address a) {
        if (const auto n = ann.nameAt(a)) return *n;
        return "sub_" + hexDigits(a, 4);
    };
    auto systemName = [&](Address a) -> std::string {
        if (const auto& k = kernalSymbols(); k.count(a)) return k.at(a);
        if (a >= 0xD000 && a < 0xE000) return C64::registerName(a);
        return "";
    };

    if (cmd == "references") {
        // what a range refers to: targets of its memory operands and calls, with name, kind and a look at the contents
        const Analysis& an = needAnalysis();
        const Address from = arg("from") ? address("from") : an.from();
        const std::uint32_t to = arg("to") ? endAddress("to") : an.to();
        std::map<Address, std::vector<Address>> targets;
        std::map<Address, bool> selfmod;
        for (const auto& [at, in] : an.code()) {
            if (at < from || at >= to) continue;
            std::optional<Address> t = in.reference;
            if (!t && in.target && (in.flow == FlowKind::Call || in.flow == FlowKind::Jump)) t = in.target;
            if (!t) continue;
            if (an.isCode(*t) && *t >= from && *t < to && an.at(*t)) continue;  // a label inside the range: the listing shows it
            targets[*t].push_back(at);
            if (Analysis::writesMemory(in) && an.isCode(*t)) selfmod[*t] = true;
        }
        for (const auto& [a, p] : an.pointerImmediates())
            if (a >= from && a < to && !ann.isConstant(a)) targets[p.first].push_back(a);
        const Memory& view = *analysisView_;
        Json arr = Json::array();
        for (auto& [t, users] : targets) {
            std::sort(users.begin(), users.end());
            users.erase(std::unique(users.begin(), users.end()), users.end());
            std::string kind = "unknown", name, preview;
            unsigned length = 0;
            const DataRegion* region = ann.dataCovering(t);
            if (selfmod.count(t)) {
                kind = "selfmod";
                const auto start = an.instructionStart(t);
                name = start ? (ann.nameAt(*start) ? *ann.nameAt(*start) : "loc_" + hexDigits(*start, 4)) + (t != *start ? "+" + std::to_string(t - *start) : "") : "";
                if (start && an.at(*start)) preview = "operand of " + an.at(*start)->text();
            } else if (an.isCode(t)) {
                kind = "code";
                name = routineName(t);
            } else if (region) {
                kind = toString(region->kind);
                length = region->length;
                name = ann.nameAt(region->address) ? *ann.nameAt(region->address) : "data_" + hexDigits(region->address, 4);
                const std::uint32_t end = static_cast<std::uint32_t>(region->address) + region->length;
                if (region->kind == DataKind::Text) {
                    for (std::uint32_t p = t; p < end && preview.size() < 60; ++p) {
                        const Byte b = view.read8(static_cast<Address>(p));
                        if (b == 0) break;
                        preview += petsciiToText(b);
                    }
                } else if (region->kind == DataKind::Screen) {
                    for (std::uint32_t p = t; p < end && preview.size() < 60; ++p) preview += screenCodeToChar(view.read8(static_cast<Address>(p)));
                } else {
                    for (std::uint32_t p = t; p < end && p < static_cast<std::uint32_t>(t) + 16; ++p)
                        preview += (preview.empty() ? "" : " ") + hexDigits(view.read8(static_cast<Address>(p)), 2);
                }
            } else if (const auto e = ann.expressionFor(t)) {
                kind = "variable";
                name = *e;
            } else if (t >= 0xD000 && t < 0xE000 && m_.c64()) {
                kind = "io";
                name = C64::registerName(t);
            } else if (kernalSymbols().count(t) && m_.c64()) {
                kind = "kernal";
                name = kernalSymbols().at(t);
            } else if (t < 0x100) {
                kind = "zeropage";
                name = "$" + hexDigits(t, 2);
            } else {
                name = an.inRange(t) ? "data_" + hexDigits(t, 4) : "$" + hexDigits(t, 4);
                if (!an.inRange(t)) kind = "external";
                for (std::uint32_t p = t; p < static_cast<std::uint32_t>(t) + 8 && p < 0x10000; ++p)
                    preview += (preview.empty() ? "" : " ") + hexDigits(view.read8(static_cast<Address>(p)), 2);
            }
            Json e = Json::object().set("addr", hex4(t)).set("name", name).set("kind", kind).set("length", length);
            if (!preview.empty()) e.set("preview", preview);
            Json usedAt = Json::array();
            for (std::size_t i = 0; i < users.size() && i < 4; ++i) usedAt.push(hex4(users[i]));
            e.set("used_at", usedAt).set("uses", users.size());
            arr.push(e);
        }
        return ok.set("references", arr);
    }

    if (cmd == "data_regions") {
        const Analysis& an = needAnalysis();
        const bool apply = flag("apply");
        const std::vector<DataGuess> guesses = an.guessData([&ann](Address a) { return ann.dataCovering(a) != nullptr; });
        Json arr = Json::array();
        unsigned applied = 0;
        for (const DataGuess& g : guesses) {
            Json e = Json::object().set("addr", hex4(g.start)).set("length", g.length).set("kind", toString(g.kind)).set("reason", g.reason);
            if (!g.preview.empty()) e.set("preview", g.preview);
            if (apply && g.kind != DataGuess::Kind::Zeros && g.kind != DataGuess::Kind::Basic) {
                // zero runs and the BASIC starter need no annotation: the listing writes them as .FILL / .BASIC
                DataRegion r;
                r.address = g.start;
                r.length = g.length;
                switch (g.kind) {
                    case DataGuess::Kind::Text: r.kind = DataKind::Text; break;
                    case DataGuess::Kind::Screen: r.kind = DataKind::Screen; break;
                    case DataGuess::Kind::Words: r.kind = DataKind::Words; break;
                    case DataGuess::Kind::Pointers: r.kind = DataKind::Pointers; break;
                    case DataGuess::Kind::Sprites: r.kind = DataKind::Sprites; break;
                    case DataGuess::Kind::Charset: r.kind = DataKind::Charset; break;
                    default: r.kind = DataKind::Bytes; break;
                }
                const bool done = !ann.setData(r).has_value();
                e.set("applied", done);
                if (done) ++applied;
            }
            arr.push(e);
        }
        Json j = ok.set("regions", arr);
        if (apply) j.set("applied", applied);
        return j;
    }

    if (cmd == "jump_tables") {
        const Analysis& an = needAnalysis();
        const auto maxEntries = static_cast<std::size_t>(number("max_entries", 64, 1, 4096));
        Json arr = Json::array();
        for (const JumpTable& t : an.jumpTables()) {
            Json e = Json::object().set("kind", kindName(t.kind));
            if (t.jump) e.set("jump", hex4(t.jump));
            e.set("table", hex4(t.table));
            if (t.split) e.set("hi_table", hex4(t.hiTable));
            e.set("minus_one", t.minusOne).set("bytes", t.bytes).set("followed", t.followed).set("entry_count", t.slots.size());
            Json entries = Json::array();
            for (std::size_t i = 0; i < t.slots.size() && i < maxEntries; ++i)
                entries.push(Json::object().set("slot", hex4(t.slots[i])).set("target", hex4(t.targets[i])).set("name", routineName(t.targets[i])));
            e.set("entries", entries);
            if (!t.followed) e.set("hint", "looks like a table of code addresses; mark it with ann_data kind=pointers and analyze again to follow it");
            arr.push(e);
        }
        return ok.set("tables", arr);
    }

    if (cmd == "routines") {
        const Analysis& an = needAnalysis();
        std::vector<RoutineInfo> list = an.routines();
        const std::string order = arg("order") ? str("order") : "bottom_up";
        if (order != "bottom_up" && order != "address") throw AnalysisError("order must be bottom_up or address");
        if (order == "bottom_up")
            std::stable_sort(list.begin(), list.end(), [](const RoutineInfo& a, const RoutineInfo& b) { return a.depth < b.depth; });
        const bool onlyUndocumented = flag("undocumented_only");
        Json out = Json::array();
        for (const RoutineInfo& r : list) {
            const RoutineDoc* doc = ann.routineAt(r.entry);
            if (onlyUndocumented && doc) continue;
            Json e = Json::object().set("addr", hex4(r.entry)).set("name", routineName(r.entry)).set("documented", doc != nullptr);
            e.set("confidence", doc ? doc->confidence : std::string());
            if (doc) e.set("summary", doc->summary).set("inputs", doc->inputs).set("outputs", doc->outputs).set("clobbers", doc->clobbers);
            e.set("start", hex4(r.start)).set("end", hex4(r.end));
            Json blocks = Json::array();
            for (const auto& [bs, be] : r.blocks) {
                Json pair = Json::array();
                pair.push(hex4(bs));
                pair.push(hex4(be));
                blocks.push(pair);
            }
            e.set("blocks", blocks).set("instructions", r.instructions).set("bytes", r.bytes).set("depth", r.depth);
            if (r.viaTable) e.set("via_table", true);
            if (const auto v = an.vectorEntries().find(r.entry); v != an.vectorEntries().end()) e.set("interrupt_vector", hex4(v->second));
            Json callers = Json::array();
            for (const Address c : r.callers) callers.push(hex4(c));
            e.set("callers", callers);
            Json callees = Json::array();
            for (const Address c : r.callees)
                callees.push(Json::object().set("addr", hex4(c)).set("name", routineName(c)).set("documented", ann.routineAt(c) != nullptr));
            e.set("callees", callees);
            Json ext = Json::array();
            for (const Address c : r.externalCalls) ext.push(hex4(c));
            e.set("external_calls", ext);
            Json kc = Json::array();
            for (const auto& [site, addr] : r.kernalCalls)
                kc.push(Json::object().set("at", hex4(site)).set("addr", hex4(addr)).set("name", Kernal::entryName(addr)));
            e.set("kernal_calls", kc);
            Json reads = Json::array(), writes = Json::array();
            for (const Address a : r.ioReads) reads.push(Json::object().set("addr", hex4(a)).set("name", C64::registerName(a)));
            for (const Address a : r.ioWrites) writes.push(Json::object().set("addr", hex4(a)).set("name", C64::registerName(a)));
            e.set("io_access", Json::object().set("reads", reads).set("writes", writes));
            Json zp = Json::array();
            for (const Address a : r.zeroPage) {
                const auto n = ann.expressionFor(a);
                zp.push(n ? Json(*n) : Json(hex2(a)));
            }
            e.set("zero_page", zp);
            Json port = Json::array();
            for (const auto& [at, value] : r.portWrites) {
                Json pj = Json::object().set("at", hex4(at));
                if (value >= 0) pj.set("value", hex2(static_cast<unsigned>(value)));
                port.push(pj);
            }
            if (!r.portWrites.empty()) e.set("port_writes", port);
            out.push(e);
        }
        (void)systemName;
        return ok.set("routines", out);
    }

    if (cmd == "listing") {
        const Analysis& an = needAnalysis();
        ListingOptions lo;
        lo.from = arg("from") ? address("from") : an.from();
        lo.to = arg("to") ? endAddress("to") : an.to();
        lo.addresses = flag("addresses");
        lo.verify = flag("verify", true);
        lo.maxChars = static_cast<std::size_t>(number("max_chars", 0, 0, 100'000'000));
        lo.c64Names = m_.c64() != nullptr;
        if (lo.from < an.from() || lo.to > an.to() || lo.to <= lo.from)
            throw AnalysisError("the range must lie inside the analysed range " + hex4(an.from()) + "-" + hex4(an.to() - 1));
        const ListingResult r = renderListing(*analysisView_, an, ann, lo);
        Json j = ok.set("listing", r.text).set("verified", r.verified).set("truncated", r.truncated).set("unnamed", r.unnamed);
        j.set("instructions", r.instructions).set("user_comments", r.userComments);
        if (!r.problem.empty()) j.set("problem", r.problem);
        return j;
    }

    // ---- annotations ----
    if (cmd == "ann_label") {
        failIf(ann.setLabel(address("addr"), str("name")));
        return ok;
    }
    if (cmd == "ann_routine") {
        RoutineDoc d;
        d.name = strOpt("name");
        d.summary = strOpt("summary");
        d.inputs = strOpt("inputs");
        d.outputs = strOpt("outputs");
        d.clobbers = strOpt("clobbers");
        d.confidence = strOpt("confidence");
        failIf(ann.setRoutine(address("addr"), d));
        return ok;
    }
    if (cmd == "ann_variable") {
        VariableDoc v;
        v.name = str("name");
        v.address = address("addr");
        v.size = static_cast<unsigned>(number("size", 1, 1, 0x10000));
        v.comment = strOpt("comment");
        failIf(ann.setVariable(v));
        return ok;
    }
    if (cmd == "ann_data") {
        DataRegion r;
        r.address = address("addr");
        r.length = static_cast<unsigned>(number("length", 0, 1, 0x10000));
        if (!arg("length")) throw AnalysisError("missing 'length'");
        const std::string kind = arg("kind") ? str("kind") : "bytes";
        const auto k = dataKindFromString(kind);
        if (!k) throw AnalysisError("kind must be bytes, words, text, screen, pointers, rts_pointers, lohi, sprites or charset");
        r.kind = *k;
        if (r.kind == DataKind::Lohi) {
            r.partner = address("partner");
            r.minusOne = flag("minus_one");
        }
        r.name = strOpt("name");
        r.comment = strOpt("comment");
        if (analysis_) {
            for (Address side : {r.address, r.kind == DataKind::Lohi ? r.partner : r.address})
                for (std::uint32_t a = side; a < static_cast<std::uint32_t>(side) + r.length && a < 0x10000; ++a)
                    if (analysis_->isCode(static_cast<Address>(a)))
                        throw AnalysisError("the range contains code (at " + hex4(a) + "); data regions must not overlap analysed instructions");
        }
        failIf(ann.setData(r));
        return ok;
    }
    if (cmd == "ann_comment") {
        const Address a = address("addr");
        if (analysis_ && analysis_->isCode(a) && !analysis_->at(a))
            throw AnalysisError("address " + hex4(a) + " is in the middle of an instruction; the comment would never appear. Use the address of the instruction's first byte");
        if (flag("block")) ann.setBlockComment(a, str("text"));
        else ann.setComment(a, str("text"));
        return ok;
    }
    if (cmd == "ann_constant") {
        const Address a = address("addr");
        if (analysis_ && !analysis_->at(a))
            throw AnalysisError("address " + hex4(a) + " is not the first byte of an analysed instruction; a constant is marked on the instruction with the immediate");
        ann.setConstant(a, !flag("remove"));
        return ok;
    }
    if (cmd == "ann_remove") {
        const Address a = address("addr");
        const bool removed = ann.removeLabel(a);
        const bool removedComment = ann.removeComment(a);
        return ok.set("removed", removed || removedComment);
    }
    if (cmd == "ann_title") {
        ann.setTitle(str("title"));
        return ok;
    }
    if (cmd == "ann_list") return ok.set("annotations", ann.toJson());
    if (cmd == "ann_save") {
        requireFiles();
        writeTextFile(str("file"), ann.toJson().dump());
        return ok;
    }
    if (cmd == "ann_load") {
        requireFiles();
        try {
            ann_ = Annotations::fromJson(Json::parse(readTextFile(str("file"))));
        } catch (const JsonError& e) {
            throw AnalysisError(std::string("cannot load annotations: ") + e.what());
        }
        return ok.set("labels", ann_.labels().size());
    }
    throw AnalysisError("unknown command '" + cmd + "'");
}

}  // namespace m6510
