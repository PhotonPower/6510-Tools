// m6510tool - command line front end for the MOS 6510 / Commodore 64 tools.
//
// Commands: asm, dis, basic list, monitor, serve, d64, t64, crt (docs/usage.md).

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "m6510/assembler.hpp"
#include "m6510/crt.hpp"
#include "m6510/d64.hpp"
#include "m6510/t64.hpp"
#include "m6510/disassembler.hpp"
#include "m6510/expression.hpp"
#include "m6510/format.hpp"
#include "m6510/image_io.hpp"
#include "m6510/monitor.hpp"
#include "m6510/petscii.hpp"
#include "m6510/session.hpp"
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
        << "  m6510tool monitor [file] [--org addr] [--c64] [--roms dir] [--no-files] [--quiet]\n"
        << "                                        interactive monitor (docs/monitor.md)\n"
        << "  m6510tool serve [--allow-files]       JSON lines on stdin/stdout (docs/agent-protocol.md)\n"
        << "  m6510tool d64 info|ls <disk.d64>      disk name, ID, free blocks / directory\n"
        << "  m6510tool d64 extract <disk.d64> <name> [-o file.prg]   a file (patterns with * and ?)\n"
        << "  m6510tool d64 boot <disk.d64> [-o file.prg]             the first program, like LOAD\"*\",8,1\n"
        << "  m6510tool d64 block <disk.d64> <track> <sector>         hex dump of a block\n"
        << "  m6510tool t64 ls <tape.t64> | t64 extract <tape.t64> <name> [-o file.prg]\n"
        << "  m6510tool crt info <cart.crt> | crt extract <cart.crt> <bank> <addr> [-o file.bin]\n"
        << "Wherever a program is read (dis, basic list, monitor, serve load_file), \"disk.d64:NAME\" and\n"
        << "\"tape.t64:NAME\" take a file out of an image.\n"
        << "  m6510tool help | version\n"
        ;
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

    const bool fromImage = splitImageSpec(input).has_value();
    const std::vector<Byte> file = fromImage ? std::vector<Byte>{} : readBinaryFile(input);
    const std::string ext = fileExtension(input);
    if (ext == ".bin" || ext == ".raw") raw = true;
    Program prog;
    if (fromImage) {
        prog = loadProgramSpec(input);
        if (org) prog.address = *org;
        if (prog.end() > 0x10000) throw UsageError("program does not fit below $FFFF");
    } else if (raw) {
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
    const Program prog = loadProgramSpec(input);
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

bool isInteractive() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

// ---- monitor ----------------------------------------------------------------------------------
// Finds ROM images in a directory by name: basic*, kernal*, char* / chargen* (e.g. the VICE file names).
std::map<std::string, std::string> findRoms(const std::string& dir) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(dir)) throw UsageError("'" + dir + "' is not a directory");
    std::map<std::string, std::string> found;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file()) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        std::string name = f.filename().string();
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto size = fs::file_size(f);
        if (name.rfind("basic", 0) == 0 && size == 0x2000) found.emplace("basic", f.string());
        else if (name.rfind("kernal", 0) == 0 && size == 0x2000) found.emplace("kernal", f.string());
        else if (name.rfind("char", 0) == 0 && size == 0x1000) found.emplace("char", f.string());
    }
    return found;
}

int cmdMonitor(Args& args) {
    MonitorOptions opt;
    std::string file, org, roms;
    bool c64 = false, quiet = false;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "--c64") c64 = true;
        else if (a == "--roms") roms = args.value(a), c64 = true;
        else if (a == "--org") org = args.value(a);
        else if (a == "--no-files") opt.allowFiles = false;
        else if (a == "--quiet" || a == "-q") quiet = true;
        else if (!a.empty() && a[0] == '-') throw UsageError("unknown option '" + a + "'");
        else if (file.empty()) file = a;
        else throw UsageError("more than one file given");
    }
    Monitor mon(opt);
    auto setup = [&](const std::string& line) {
        const std::string out = mon.execute(line);
        if (out.rfind("? ", 0) == 0) throw UsageError(out.substr(2, out.size() - 3));
        return out;
    };
    std::string startup;
    if (c64) {
        std::map<std::string, std::string> found;
        if (!roms.empty()) {
            found = findRoms(roms);
            if (found.empty()) throw UsageError("no ROM images (basic*, kernal*, char*) in '" + roms + "'");
        }
        setup(found.count("kernal") ? "machine c64 rom" : "machine c64");
        // The ROM files bypass the monitor's file switch: they are part of the command line, not a monitor command.
        for (const auto& [which, path] : found) {
            mon.machine().loadRom(which, readBinaryFile(path));
            startup += which + " ROM: " + path + "\n";
        }
    }
    if (!file.empty()) {
        const std::optional<Address> at = org.empty() ? std::nullopt : std::optional<Address>(parseAddress(org, "--org"));
        const LoadedFile f = mon.machine().loadFile(file, "auto", at);
        startup += "loaded " + file + ": " + hexDigits(f.program.address, 4) + "-" +
                   hexDigits(static_cast<unsigned>(f.program.bytes.empty() ? f.program.address : f.program.end() - 1), 4) +
                   ", PC=" + hexDigits(f.entry, 4) + (f.basic.empty() ? "" : "  (BASIC: " + f.basic + ")") + "\n";
        (void)setup("d " + hexDigits(f.entry, 4) + " 1");  // continue the disassembly at the entry
    }
    const bool interactive = !quiet && isInteractive();
    if (interactive) std::cout << Monitor::banner();
    std::cout << startup;
    if (interactive) std::cout << mon.execute("r");
    std::string line;
    while (!mon.finished()) {
        if (interactive) std::cout << mon.prompt() << " " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::cout << mon.execute(line) << std::flush;
    }
    return kExitOk;
}

// ---- serve ------------------------------------------------------------------------------------
int cmdServe(Args& args) {
    SessionOptions opt;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "--allow-files") opt.allowFiles = true;
        else throw UsageError("unknown option '" + a + "'");
    }
    Session session(opt);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        std::cout << session.handleLine(line) << std::endl;
    }
    return kExitOk;
}

// ---- images ------------------------------------------------------------------------------------
std::string outputName(Args& args, const std::string& fallback) {
    std::string out = fallback;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "-o") out = args.value(a);
        else throw UsageError("unknown option '" + a + "'");
    }
    return out;
}

std::string safeFileName(const std::string& name) {
    std::string s;
    for (char c : name) s += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    return s.empty() ? "file" : s;
}

void printProblems(const std::vector<std::string>& problems) {
    for (const std::string& p : problems) std::cerr << "warning: " << p << "\n";
}

int cmdD64(Args& args) {
    if (args.done()) throw UsageError("d64 needs a subcommand: info, ls, extract, boot, block");
    const std::string sub = args.next();
    if (args.done()) throw UsageError("d64 " + sub + " needs a disk image");
    const std::string path = args.next();
    const D64Image disk(readBinaryFile(path));
    if (sub == "info") {
        if (!args.done()) throw UsageError("too many arguments");
        std::cout << "name:   \"" << disk.diskName() << "\"\nid:     " << disk.diskId() << "\ndos:    " << disk.dosType() << "\n"
                  << "tracks: " << disk.tracks() << (disk.hasErrorBytes() ? " (with error bytes, " + std::to_string(disk.badSectors()) + " bad sectors)" : "")
                  << "\nfree:   " << disk.freeBlocks() << " blocks\nfiles:  " << disk.directory().size() << "\n";
        printProblems(disk.problems());
        return kExitOk;
    }
    if (sub == "ls") {
        if (!args.done()) throw UsageError("too many arguments");
        std::cout << "0 \"" << disk.diskName() << "\" " << disk.diskId() << " " << disk.dosType() << "\n";
        for (const D64Entry& e : disk.directory()) {
            std::string blocks = std::to_string(e.blocks);
            blocks.append(blocks.size() < 5 ? 5 - blocks.size() : 1, ' ');
            std::string name = "\"" + e.name + "\"";
            name.append(name.size() < 19 ? 19 - name.size() : 1, ' ');
            std::cout << blocks << name << (e.closed ? " " : "*") << e.type << (e.locked ? "<" : "") << "\n";
        }
        std::cout << disk.freeBlocks() << " blocks free.\n";
        printProblems(disk.problems());
        return kExitOk;
    }
    if (sub == "extract" || sub == "boot") {
        std::string pattern = "*";
        if (sub == "extract") {
            if (args.done()) throw UsageError("d64 extract needs the name of the file (patterns with * and ?)");
            pattern = args.next();
        }
        const auto entry = disk.find(pattern);
        if (!entry) throw UsageError("no file matching \"" + pattern + "\" on " + path);
        if (sub == "boot" && entry->type != "PRG") throw UsageError("the first file is not a program");
        const std::vector<Byte> bytes = disk.readFile(*entry);
        const std::string out = outputName(args, safeFileName(entry->name) + (entry->type == "PRG" ? ".prg" : "." + entry->type));
        writeBinaryFile(out, bytes);
        std::cout << "\"" << entry->name << "\" (" << entry->type << ", " << entry->blocks << " blocks, " << bytes.size() << " bytes";
        if (entry->type == "PRG" && bytes.size() >= 2) std::cout << ", load address $" << hexDigits(static_cast<unsigned>(bytes[0] | (bytes[1] << 8)), 4);
        std::cout << ") -> " << out << "\n";
        printProblems(disk.problems());
        return kExitOk;
    }
    if (sub == "block") {
        if (args.done()) throw UsageError("d64 block needs track and sector");
        const auto t = static_cast<unsigned>(parseNumber(args.next(), "track"));
        if (args.done()) throw UsageError("d64 block needs track and sector");
        const auto s = static_cast<unsigned>(parseNumber(args.next(), "sector"));
        if (!args.done()) throw UsageError("too many arguments");
        if (!disk.validBlock(t, s)) throw UsageError("track " + std::to_string(t) + " sector " + std::to_string(s) + " is not on the disk");
        const std::vector<Byte> b = disk.block(t, s);
        for (unsigned row = 0; row < 256; row += 16) {
            std::cout << hexDigits(row, 2) << ": ";
            std::string text;
            for (unsigned i = 0; i < 16; ++i) {
                std::cout << hexDigits(b[row + i], 2) << ' ';
                text += b[row + i] >= 0x20 && b[row + i] < 0x7F ? static_cast<char>(b[row + i]) : '.';
            }
            std::cout << ' ' << text << "\n";
        }
        if (const auto e = disk.errorCode(t, s)) std::cout << "error code: " << static_cast<unsigned>(*e) << "\n";
        return kExitOk;
    }
    throw UsageError("unknown subcommand '" + sub + "' (info, ls, extract, boot, block)");
}

int cmdT64(Args& args) {
    if (args.done()) throw UsageError("t64 needs a subcommand: ls, extract");
    const std::string sub = args.next();
    if (args.done()) throw UsageError("t64 " + sub + " needs a tape image");
    const std::string path = args.next();
    const T64Image tape(readBinaryFile(path));
    if (sub == "ls") {
        if (!args.done()) throw UsageError("too many arguments");
        std::cout << "\"" << tape.tapeName() << "\" (version " << hexDigits(tape.version(), 4) << ")\n";
        for (const T64Entry& e : tape.entries())
            std::cout << "  \"" << e.name << "\"  $" << hexDigits(e.start, 4) << "-$" << hexDigits(static_cast<unsigned>(e.start + e.length - 1), 4) << "  "
                      << e.length << " bytes\n";
        printProblems(tape.problems());
        return kExitOk;
    }
    if (sub == "extract") {
        if (args.done()) throw UsageError("t64 extract needs the name of the file (patterns with * and ?)");
        const std::string pattern = args.next();
        const auto entry = tape.find(pattern);
        if (!entry) throw UsageError("no file matching \"" + pattern + "\" in " + path);
        const Program p = tape.readProgram(*entry);
        const std::string out = outputName(args, safeFileName(entry->name) + ".prg");
        writeBinaryFile(out, makePrg(p.address, p.bytes));
        std::cout << "\"" << entry->name << "\" ($" << hexDigits(p.address, 4) << ", " << p.bytes.size() << " bytes) -> " << out << "\n";
        printProblems(tape.problems());
        return kExitOk;
    }
    throw UsageError("unknown subcommand '" + sub + "' (ls, extract)");
}

int cmdCrt(Args& args) {
    if (args.done()) throw UsageError("crt needs a subcommand: info, extract");
    const std::string sub = args.next();
    if (args.done()) throw UsageError("crt " + sub + " needs a cartridge image");
    const std::string path = args.next();
    const CrtImage crt(readBinaryFile(path));
    if (sub == "info") {
        if (!args.done()) throw UsageError("too many arguments");
        std::cout << "name:  \"" << crt.name() << "\"\ntype:  " << crt.hardwareType() << " (" << crt.hardwareName() << ")\nmode:  " << crt.mode()
                  << " (EXROM=" << crt.exrom() << " GAME=" << crt.game() << ")\nchips: " << crt.chips().size() << "\n";
        for (const CrtChip& c : crt.chips())
            std::cout << "  bank " << c.bank << "  $" << hexDigits(c.load, 4) << "-$" << hexDigits(static_cast<unsigned>(c.load + c.data.size() - 1), 4)
                      << "  " << (c.type == 0 ? "ROM" : c.type == 1 ? "RAM" : "flash") << "\n";
        if (crt.hardwareType() != 0) std::cout << "only type 0 can run in the emulator; `crt extract` writes the banks for the disassembler\n";
        return kExitOk;
    }
    if (sub == "extract") {
        if (args.done()) throw UsageError("crt extract needs a bank and a load address");
        const auto bank = static_cast<unsigned>(parseNumber(args.next(), "bank"));
        if (args.done()) throw UsageError("crt extract needs a bank and a load address");
        const Address load = parseAddress(args.next(), "load address");
        for (const CrtChip& c : crt.chips()) {
            if (c.bank != bank || c.load != load) continue;
            const std::string out = outputName(args, "bank" + std::to_string(bank) + "_" + hexDigits(load, 4) + ".bin");
            writeBinaryFile(out, c.data);
            std::cout << "bank " << bank << " at $" << hexDigits(load, 4) << ", " << c.data.size() << " bytes -> " << out
                      << "  (m6510tool dis " << out << " --org " << load << ")\n";
            return kExitOk;
        }
        throw UsageError("no CHIP packet with bank " + std::to_string(bank) + " at $" + hexDigits(load, 4));
    }
    throw UsageError("unknown subcommand '" + sub + "' (info, extract)");
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
        if (command == "monitor") return cmdMonitor(args);
        if (command == "d64") return cmdD64(args);
        if (command == "t64") return cmdT64(args);
        if (command == "crt") return cmdCrt(args);
        if (command == "serve") return cmdServe(args);
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
