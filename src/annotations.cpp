#include "m6510/annotations.hpp"

#include <algorithm>
#include <cctype>

#include "m6510/assembler.hpp"
#include "m6510/c64.hpp"
#include "m6510/disassembler.hpp"
#include "m6510/format.hpp"
#include "m6510/kernal.hpp"

namespace m6510 {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string hex4(Address a) { return "$" + hexDigits(a, 4); }

// KERNAL entry names and chip register names: the listing defines them itself.
const std::set<std::string>& systemNames() {
    static const std::set<std::string> names = [] {
        std::set<std::string> n;
        for (const auto& [a, name] : Kernal::jumpTable()) n.insert(upper(name));
        for (const auto& [a, name] : kernalSymbols()) n.insert(upper(name));
        for (unsigned a = 0xD000; a < 0xE000; ++a) {
            const std::string r = C64::registerName(static_cast<Address>(a));
            if (!r.empty()) n.insert(upper(r));
        }
        for (const char* r : {"S", "P", "PC", "SP"}) n.insert(r);
        return n;
    }();
    return names;
}

bool isPlaceholder(const std::string& name) {
    // sub_XXXX / loc_XXXX / data_XXXX are generated for unnamed addresses
    for (const std::string p : {"sub_", "loc_", "data_"}) {
        if (name.size() == p.size() + 4 && upper(name.substr(0, p.size())) == upper(p)) {
            bool allHex = true;
            for (std::size_t i = p.size(); i < name.size(); ++i) allHex = allHex && std::isxdigit(static_cast<unsigned char>(name[i]));
            if (allHex) return true;
        }
    }
    return false;
}

}  // namespace

const char* toString(DataKind k) {
    switch (k) {
        case DataKind::Bytes: return "bytes";
        case DataKind::Words: return "words";
        case DataKind::Text: return "text";
        case DataKind::Screen: return "screen";
        case DataKind::Pointers: return "pointers";
        case DataKind::RtsPointers: return "rts_pointers";
        case DataKind::Lohi: return "lohi";
        case DataKind::Sprites: return "sprites";
        case DataKind::Charset: return "charset";
    }
    return "bytes";
}

std::optional<DataKind> dataKindFromString(const std::string& t) {
    for (DataKind k : {DataKind::Bytes, DataKind::Words, DataKind::Text, DataKind::Screen, DataKind::Pointers, DataKind::RtsPointers,
                       DataKind::Lohi, DataKind::Sprites, DataKind::Charset})
        if (t == toString(k)) return k;
    return std::nullopt;
}

std::optional<std::string> Annotations::checkName(const std::string& name) {
    if (name.empty()) return "the name is empty";
    if (name.size() > 64) return "the name is longer than 64 characters";
    if (name[0] == '.') return "a name must start with a letter or '_' (local labels are not used in listings)";
    std::string why;
    if (!isValidSymbolName(name, &why)) return why;
    const std::string u = upper(name);
    if (systemNames().count(u)) return "'" + name + "' is reserved (register, KERNAL entry or chip register name)";
    if (isPlaceholder(name)) return "'" + name + "' looks like a generated placeholder (sub_/loc_/data_ + 4 hex digits)";
    return std::nullopt;
}

std::optional<std::string> Annotations::claimName(Address address, const std::string& name) {
    if (auto err = checkName(name)) return err;
    const std::string key = upper(name);
    if (const auto it = byName_.find(key); it != byName_.end() && it->second != address)
        return "the name '" + name + "' is already used for " + hex4(it->second);
    if (const auto old = labels_.find(address); old != labels_.end()) byName_.erase(upper(old->second));
    labels_[address] = name;
    byName_[key] = address;
    // a name is the name of everything documented at that address: keep the records consistent
    if (const auto it = routines_.find(address); it != routines_.end()) it->second.name = name;
    for (VariableDoc& v : variables_)
        if (v.address == address) v.name = name;
    if (const auto it = data_.find(address); it != data_.end()) it->second.name = name;
    return std::nullopt;
}

std::optional<std::string> Annotations::setLabel(Address address, const std::string& name) { return claimName(address, name); }

std::optional<std::string> Annotations::setRoutine(Address address, const RoutineDoc& doc) {
    if (!doc.confidence.empty() && doc.confidence != "high" && doc.confidence != "medium" && doc.confidence != "low")
        return "confidence must be high, medium or low";
    if (!doc.name.empty()) {
        if (auto err = claimName(address, doc.name)) return err;
    }
    RoutineDoc d = doc;
    if (d.name.empty()) {
        if (const auto n = nameAt(address)) d.name = *n;
    }
    routines_[address] = d;
    return std::nullopt;
}

std::optional<std::string> Annotations::setVariable(const VariableDoc& v) {
    if (v.size < 1 || v.size > 0x10000 || v.address + v.size > 0x10000) return "size must be 1..65536 and stay below $10000";
    if (auto err = claimName(v.address, v.name)) return err;
    for (VariableDoc& x : variables_) {
        if (x.address == v.address) {
            x = v;
            return std::nullopt;
        }
    }
    variables_.push_back(v);
    return std::nullopt;
}

std::optional<std::string> Annotations::setData(const DataRegion& r) {
    if (r.length < 1 || r.address + r.length > 0x10000) return "length must be at least 1 and the region must end below $10000";
    if ((r.kind == DataKind::Words || r.kind == DataKind::Pointers || r.kind == DataKind::RtsPointers) && r.length % 2)
        return "word and pointer data needs an even length";
    if (r.kind == DataKind::Lohi) {
        if (r.partner + r.length > 0x10000) return "the table of high bytes must end below $10000";
        if (r.partner < r.address + r.length && r.address < r.partner + r.length) return "the two tables of a lohi region overlap";
    }
    // a new region may replace one that starts at the same address, but not overlap others
    for (const auto& [a, old] : data_) {
        if (a == r.address) continue;
        for (Address x : {r.address, r.kind == DataKind::Lohi ? r.partner : r.address}) {
            for (unsigned i = 0; i < r.length; ++i) {
                if (old.covers(static_cast<Address>(x + i))) return "overlaps the data region at " + hex4(a);
            }
        }
    }
    if (!r.name.empty()) {
        if (auto err = claimName(r.address, r.name)) return err;
    }
    data_[r.address] = r;
    return std::nullopt;
}

bool Annotations::removeLabel(Address address) {
    bool any = false;
    if (const auto it = labels_.find(address); it != labels_.end()) {
        byName_.erase(upper(it->second));
        labels_.erase(it);
        any = true;
    }
    any = routines_.erase(address) > 0 || any;
    const std::size_t before = variables_.size();
    variables_.erase(std::remove_if(variables_.begin(), variables_.end(), [&](const VariableDoc& v) { return v.address == address; }),
                     variables_.end());
    any = variables_.size() != before || any;
    any = data_.erase(address) > 0 || any;
    return any;
}

bool Annotations::removeComment(Address address) {
    return comments_.erase(address) + blockComments_.erase(address) + constants_.erase(address) > 0;
}

void Annotations::clear() { *this = Annotations{}; }

std::optional<std::string> Annotations::nameAt(Address a) const {
    const auto it = labels_.find(a);
    if (it == labels_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> Annotations::expressionFor(Address a) const {
    if (const auto n = nameAt(a)) return n;
    auto plus = [](Address off) { return off < 10 ? std::to_string(off) : "$" + hexDigits(off, 1); };
    for (const VariableDoc& v : variables_)
        if (a > v.address && a < v.address + v.size) return v.name + "+" + plus(static_cast<Address>(a - v.address));
    for (const auto& [start, r] : data_)
        if (!r.name.empty() && a > start && a < start + r.length) return r.name + "+" + plus(static_cast<Address>(a - start));
    return std::nullopt;
}

std::optional<Address> Annotations::addressOf(const std::string& name) const {
    const auto it = byName_.find(upper(name));
    if (it == byName_.end()) return std::nullopt;
    return it->second;
}

const RoutineDoc* Annotations::routineAt(Address a) const {
    const auto it = routines_.find(a);
    return it == routines_.end() ? nullptr : &it->second;
}
const std::string* Annotations::commentAt(Address a) const {
    const auto it = comments_.find(a);
    return it == comments_.end() ? nullptr : &it->second;
}
const std::string* Annotations::blockCommentAt(Address a) const {
    const auto it = blockComments_.find(a);
    return it == blockComments_.end() ? nullptr : &it->second;
}
const DataRegion* Annotations::dataAt(Address a) const {
    if (const auto it = data_.find(a); it != data_.end()) return &it->second;
    for (const auto& [start, r] : data_)
        if (r.kind == DataKind::Lohi && r.partner == a) return &r;
    return nullptr;
}
const DataRegion* Annotations::dataCovering(Address a) const {
    for (const auto& [start, r] : data_)
        if (r.covers(a)) return &r;
    return nullptr;
}
const VariableDoc* Annotations::variableAt(Address a) const {
    for (const VariableDoc& v : variables_)
        if (v.address == a) return &v;
    return nullptr;
}

namespace {
std::string hexAddr(Address a) { return "0x" + hexDigits(a, 4); }
Address parseAddr(const Json& j) {
    std::int64_t v = -1;
    if (j.isNumber()) v = j.asInt();
    else if (j.isString()) {
        try {
            v = static_cast<std::int64_t>(std::stoul(j.asString(), nullptr, 0));
        } catch (const std::exception&) {
            v = -1;
        }
    }
    if (v < 0 || v > 0xFFFF) throw JsonError("bad address in annotations");
    return static_cast<Address>(v);
}
const Json& need(const Json& j, const char* key) {
    const Json* v = j.find(key);
    if (!v) throw JsonError(std::string("annotations: missing field '") + key + "'");
    return *v;
}
std::string getStr(const Json& j, const char* key) {
    const Json* v = j.find(key);
    return v && v->isString() ? v->asString() : std::string();
}
std::int64_t getInt(const Json& j, const char* key, std::int64_t def) {
    const Json* v = j.find(key);
    return v && v->isNumber() ? v->asInt() : def;
}
}  // namespace

Json Annotations::toJson() const {
    Json j = Json::object();
    j.set("version", 1);
    j.set("title", title_);
    Json labels = Json::array();
    for (const auto& [a, n] : labels_) labels.push(Json::object().set("addr", hexAddr(a)).set("name", n));
    j.set("labels", labels);
    Json routines = Json::array();
    for (const auto& [a, r] : routines_) {
        routines.push(Json::object().set("addr", hexAddr(a)).set("name", r.name).set("summary", r.summary).set("inputs", r.inputs)
                          .set("outputs", r.outputs).set("clobbers", r.clobbers).set("confidence", r.confidence));
    }
    j.set("routines", routines);
    Json vars = Json::array();
    for (const VariableDoc& v : variables_)
        vars.push(Json::object().set("name", v.name).set("addr", hexAddr(v.address)).set("size", v.size).set("comment", v.comment));
    j.set("variables", vars);
    Json data = Json::array();
    for (const auto& [a, r] : data_) {
        Json e = Json::object().set("addr", hexAddr(a)).set("length", r.length).set("kind", toString(r.kind));
        if (r.kind == DataKind::Lohi) e.set("partner", hexAddr(r.partner)).set("minus_one", r.minusOne);
        e.set("name", r.name).set("comment", r.comment);
        data.push(e);
    }
    j.set("data", data);
    Json comments = Json::array();
    for (const auto& [a, t] : comments_) comments.push(Json::object().set("addr", hexAddr(a)).set("text", t));
    j.set("comments", comments);
    Json blocks = Json::array();
    for (const auto& [a, t] : blockComments_) blocks.push(Json::object().set("addr", hexAddr(a)).set("text", t));
    j.set("block_comments", blocks);
    Json constants = Json::array();
    for (const Address a : constants_) constants.push(Json::object().set("addr", hexAddr(a)));
    j.set("constants", constants);
    return j;
}

Annotations Annotations::fromJson(const Json& j) {
    if (!j.isObject()) throw JsonError("annotations must be a JSON object");
    Annotations a;
    auto fail = [](const std::string& what, const std::string& why) { throw JsonError(what + ": " + why); };
    auto list = [&](const char* key) -> const std::vector<Json>& {
        static const std::vector<Json> none;
        const Json* l = j.find(key);
        if (!l || l->isNull()) return none;
        if (!l->isArray()) throw JsonError(std::string("annotations: '") + key + "' must be an array");
        for (const Json& e : l->items())
            if (!e.isObject()) throw JsonError(std::string("annotations: the entries of '") + key + "' must be objects");
        return l->items();
    };
    a.title_ = getStr(j, "title");
    for (const Json& e : list("labels"))
        if (auto err = a.setLabel(parseAddr(need(e, "addr")), getStr(e, "name"))) fail("label", *err);
    for (const Json& e : list("routines")) {
        RoutineDoc d;
        d.name = getStr(e, "name");
        d.summary = getStr(e, "summary");
        d.inputs = getStr(e, "inputs");
        d.outputs = getStr(e, "outputs");
        d.clobbers = getStr(e, "clobbers");
        d.confidence = getStr(e, "confidence");
        if (auto err = a.setRoutine(parseAddr(need(e, "addr")), d)) fail("routine", *err);
    }
    for (const Json& e : list("variables")) {
        VariableDoc v;
        v.name = getStr(e, "name");
        v.address = parseAddr(need(e, "addr"));
        const std::int64_t size = getInt(e, "size", 1);
        if (size < 1 || size > 0x10000) fail("variable", "bad size");
        v.size = static_cast<unsigned>(size);
        v.comment = getStr(e, "comment");
        if (auto err = a.setVariable(v)) fail("variable", *err);
    }
    for (const Json& e : list("data")) {
        DataRegion r;
        r.address = parseAddr(need(e, "addr"));
        const std::int64_t length = getInt(e, "length", 0);
        if (length < 1 || length > 0x10000) fail("data", "bad length");
        r.length = static_cast<unsigned>(length);
        const auto kind = dataKindFromString(getStr(e, "kind"));
        if (!kind) fail("data", "unknown kind '" + getStr(e, "kind") + "'");
        r.kind = *kind;
        if (r.kind == DataKind::Lohi) {
            r.partner = parseAddr(need(e, "partner"));
            const Json* m = e.find("minus_one");
            r.minusOne = m && m->isBool() && m->asBool();
        }
        r.name = getStr(e, "name");
        r.comment = getStr(e, "comment");
        if (auto err = a.setData(r)) fail("data", *err);
    }
    for (const Json& e : list("comments")) a.setComment(parseAddr(need(e, "addr")), getStr(e, "text"));
    for (const Json& e : list("block_comments")) a.setBlockComment(parseAddr(need(e, "addr")), getStr(e, "text"));
    for (const Json& e : list("constants")) a.setConstant(parseAddr(need(e, "addr")), true);
    return a;
}

}  // namespace m6510
