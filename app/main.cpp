// m6510tool - command line front end for the MOS 6510 / Commodore 64 tools.
//
// Implemented: asm, dis, basic list. Planned (see docs/roadmap.md): monitor, serve, d64, t64, crt.

#include <cstdio>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "m6510/assembler.hpp"
#include "m6510/disassembler.hpp"
#include "m6510/expression.hpp"
#include "m6510/format.hpp"
#include "m6510/image_io.hpp"
#include "m6510/petscii.hpp"
#include "m6510/version.hpp"

namespace {

using namespace m6510;

constexpr int kExitOk = 0;
constexpr int kExitError = 1;  // errors in the source / input file
constexpr int kExitUsage = 2;  // bad command line / I/O problems

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void printUsage(std::ostream& out) {
    out << "m6510tool " << kVersion << " - MOS 6510 / Commodore 64 tools\n"
        << "\n"
        << "Usage:\n"
        << "  m6510tool asm <source.asm> [-o out.prg] [--raw] [-l listing|-] [-s symbols] [--vice-labels file]\n"
        << "                [--org addr] [--no-illegal | --unstable] [-D name=value]\n"
        << "  m6510tool dis <file> [--org addr] [--raw] [--from a] [--to b] [-e entry]... [--linear]\n"
        << "                [-c comments.txt] [--asm] [--no-bytes] [--style dollar|c|h] [--illegal none|stable|all]\n"
        << "                [--symbols file] [--no-kernal-names] [-o out]\n"
        << "  m6510tool basic list <file.prg>\n"
        << "  m6510tool help | version\n"
        << "\n"
        << "Planned (not implemented yet, see docs/roadmap.md):\n"
        << "  m6510tool monitor [image]\n"
        << "  m6510tool serve [--allow-files]\n"
        << "  m6510tool d64 | t64 | crt ...\n";
}

std::int64_t parseNumber(const std::string& text, const char* what) {
    const ExprResult r = ExpressionEvaluator(nullptr, 0).evaluate(text);
    if (!r.ok || r.undefined) throw UsageError(std::string("bad ") + what + " '" + text + "'");
    return r.value;
}

Address parseAddress(const std::string& text, const char* what) {
    const std::int64_t v = parseNumber(text, what);
    if (v < 0 || v > 0xFFFF) throw UsageError(std::string(what) + " out of range (0..$FFFF): " + text);
    return static_cast<Address>(v);
}

// Simple argument cursor.
struct Args {
    std::vector<std::string> v;
    std::size_t i = 0;
    [[nodiscard]] bool done() const { return i >= v.size(); }
    std::string next() { return v[i++]; }
    std::string value(const std::string& option) {
        if (done()) throw UsageError("option " + option + " needs a value");
        return next();
    }
};

void writeOutput(const std::string& path, const std::string& text) {
    if (path == "-") std::cout << text;
    else writeTextFile(path, text);
}

std::string replaceExtension(const std::string& path, const std::string& ext) {
    const std::string e = fileExtension(path);
    return (e.empty() ? path : path.substr(0, path.size() - e.size())) + ext;
}

// ---- asm --------------------------------------------------------------------------------------
int cmdAsm(Args& args) {
    std::string source, out, listing, symbols, vice;
    bool raw = false;
    AssemblerOptions opt;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "-o") out = args.value(a);
        else if (a == "--raw") raw = true;
        else if (a == "-l") listing = args.value(a);
        else if (a == "-s") symbols = args.value(a);
        else if (a == "--vice-labels") vice = args.value(a);
        else if (a == "--org") opt.origin = parseAddress(args.value(a), "origin");
        else if (a == "--no-illegal") opt.level = CpuLevel::Documented;
        else if (a == "--unstable") opt.level = CpuLevel::All;
        else if (a == "-D") {
            const std::string d = args.value(a);
            const std::size_t eq = d.find('=');
            const std::string name = d.substr(0, eq);
            std::string why;
            if (!isValidSymbolName(name, &why)) throw UsageError("-D: " + why);
            opt.predefined[name] = eq == std::string::npos ? 1 : parseNumber(d.substr(eq + 1), "value");
        } else if (!a.empty() && a[0] == '-') throw UsageError("unknown option '" + a + "'");
        else if (source.empty()) source = a;
        else throw UsageError("more than one source file");
    }
    if (source.empty()) throw UsageError("asm needs a source file");
    if (out.empty()) out = replaceExtension(source, raw ? ".bin" : ".prg");

    const AssemblyResult r = assemble(readTextFile(source), opt);
    for (const Diagnostic& d : r.errors) std::cerr << source << ":" << d.line << ": error: " << d.message << "\n";
    if (!listing.empty()) writeOutput(listing, r.formatListing());
    if (!r.ok()) {
        std::cerr << source << ": " << r.errors.size() << " error(s), no output written\n";
        return kExitError;
    }
    if (!symbols.empty()) writeOutput(symbols, r.formatSymbols());
    if (!vice.empty()) writeOutput(vice, r.formatViceLabels());
    const std::vector<Byte> image = r.flatten();
    writeBinaryFile(out, raw ? image : makePrg(r.lowest(), image));
    std::ostream& info = (listing == "-" || symbols == "-" || vice == "-") ? std::cerr : std::cout;
    info << source << ": " << image.size() << " bytes";
    if (!image.empty()) {
        info << ", " << formatHex(r.lowest(), 4, NumberStyle::Dollar) << " - "
             << formatHex(r.end() - 1, 4, NumberStyle::Dollar);
    }
    info << ", 0 error(s)\n";
    return kExitOk;
}

// ---- dis --------------------------------------------------------------------------------------
int cmdDis(Args& args) {
    std::string input, comments, symbolFile, out = "-";
    std::optional<Address> org, from, to;
    std::vector<Address> entries;
    bool raw = false, linear = false, kernalNames = true;
    DisassemblerOptions dopt;
    CpuLevel level = CpuLevel::Stable;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "--org") org = parseAddress(args.value(a), "origin");
        else if (a == "--raw") raw = true;
        else if (a == "--from") from = parseAddress(args.value(a), "address");
        else if (a == "--to") to = parseAddress(args.value(a), "address");
        else if (a == "-e") entries.push_back(parseAddress(args.value(a), "entry"));
        else if (a == "--linear") linear = true;
        else if (a == "-c") comments = args.value(a);
        else if (a == "--asm") dopt.asmSource = true;
        else if (a == "--no-bytes") dopt.showBytes = false;
        else if (a == "--no-xrefs") dopt.xrefs = false;
        else if (a == "--style") {
            if (!parseNumberStyle(args.value(a), dopt.style)) throw UsageError("--style: dollar, c or h");
        } else if (a == "--illegal") {
            const std::string v = args.value(a);
            if (v == "none") level = CpuLevel::Documented;
            else if (v == "stable") level = CpuLevel::Stable;
            else if (v == "all") level = CpuLevel::All;
            else throw UsageError("--illegal: none, stable or all");
        } else if (a == "--symbols") symbolFile = args.value(a);
        else if (a == "--no-kernal-names") kernalNames = false;
        else if (a == "-o") out = args.value(a);
        else if (!a.empty() && a[0] == '-') throw UsageError("unknown option '" + a + "'");
        else if (input.empty()) input = a;
        else throw UsageError("more than one input file");
    }
    if (input.empty()) throw UsageError("dis needs an input file");

    const std::vector<Byte> file = readBinaryFile(input);
    const std::string ext = fileExtension(input);
    if (ext == ".bin" || ext == ".raw") raw = true;
    Program prog;
    if (raw) {
        if (!org) throw UsageError("a raw file needs --org");
        prog.address = *org;
        prog.bytes = file;
        if (prog.end() > 0x10000) throw UsageError("raw file does not fit between --org and $FFFF");
    } else {
        prog = looksLikeP00(file) ? parseP00(file) : parsePrg(file);
        if (org) {
            prog.address = *org;
            if (prog.end() > 0x10000) throw UsageError("program does not fit between --org and $FFFF");
        }
    }
    Memory mem;
    loadProgram(mem, prog);
    const Address start = from.value_or(prog.address);
    const std::uint32_t end = to ? std::uint32_t{*to} + 1 : prog.end();
    if (end <= start) throw UsageError("empty address range");

    Disassembler dis(mem, start, end, level);
    if (kernalNames)
        for (const auto& [a, n] : kernalSymbols()) dis.addSymbol(a, n);
    if (!symbolFile.empty()) {
        std::map<Address, std::string> syms;
        for (const Diagnostic& d : parseSymbolFile(readTextFile(symbolFile), syms))
            std::cerr << symbolFile << ":" << d.line << ": " << d.message << "\n";
        for (const auto& [a, n] : syms) dis.addSymbol(a, n);
    }
    for (Address e : entries) dis.addEntry(e);
    if (!comments.empty()) {
        for (const Diagnostic& d : loadCommentFile(readTextFile(comments), dis))
            std::cerr << comments << ":" << d.line << ": " << d.message << "\n";
    }
    if (linear) dis.analyzeLinear();
    else dis.analyzeFlow();
    writeOutput(out, dis.render(dopt));
    return kExitOk;
}

// ---- basic ------------------------------------------------------------------------------------
int cmdBasic(Args& args) {
    if (args.done()) throw UsageError("basic needs a sub-command (list)");
    const std::string sub = args.next();
    if (sub != "list") throw UsageError("unknown basic sub-command '" + sub + "'");
    if (args.done()) throw UsageError("basic list needs a file");
    const std::string input = args.next();
    const Program prog = loadProgramFile(input);
    Memory mem;
    loadProgram(mem, prog);
    std::string error;
    for (const BasicLine& l : detokenizeBasic(mem, prog.address, &error)) std::cout << l.number << " " << l.text << "\n";
    if (!error.empty()) {
        std::cerr << input << ": " << error << "\n";
        return kExitError;
    }
    return kExitOk;
}

bool isPlanned(const std::string& command) {
    for (const char* c : {"monitor", "serve", "d64", "t64", "crt"}) {
        if (command == c) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(std::cerr);
        return kExitUsage;
    }
    const std::string command = argv[1];
    Args args;
    for (int i = 2; i < argc; ++i) args.v.emplace_back(argv[i]);
    try {
        if (command == "help" || command == "--help" || command == "-h") {
            printUsage(std::cout);
            return kExitOk;
        }
        if (command == "version" || command == "--version") {
            std::cout << "m6510tool " << kVersion << "\n";
            return kExitOk;
        }
        if (command == "asm") return cmdAsm(args);
        if (command == "dis") return cmdDis(args);
        if (command == "basic") return cmdBasic(args);
        if (isPlanned(command)) {
            std::cerr << "m6510tool: '" << command << "' is not implemented yet (see docs/roadmap.md)\n";
            return kExitUsage;
        }
        std::cerr << "m6510tool: unknown command '" << command << "'\n\n";
        printUsage(std::cerr);
        return kExitUsage;
    } catch (const UsageError& e) {
        std::cerr << "m6510tool " << command << ": " << e.what() << "\n";
        return kExitUsage;
    } catch (const IoError& e) {
        std::cerr << "m6510tool " << command << ": " << e.what() << "\n";
        return kExitUsage;
    }
}
