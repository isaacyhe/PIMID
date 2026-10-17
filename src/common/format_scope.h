#pragma once
// 1.12.15: scoped stream formatting. Through 1.12.14 statements set std::fixed / std::setprecision on std::cout and
// the zsim configuration streams and never restored them, so later numbers printed under a format nobody chose.
#include <ios>
#include <ostream>

namespace pimid {

// Restores a stream's flags and precision when it goes out of scope.
class StreamFormatGuard {
public:
    explicit StreamFormatGuard(std::ostream& os) : os_(os), flags_(os.flags()), prec_(os.precision()) {}
    ~StreamFormatGuard() { os_.flags(flags_); os_.precision(prec_); }
    StreamFormatGuard(const StreamFormatGuard&) = delete;
    StreamFormatGuard& operator=(const StreamFormatGuard&) = delete;
private:
    std::ostream& os_;
    std::ios_base::fmtflags flags_;
    std::streamsize prec_;
};

// One statement's formatting: pimid::fmt_scoped(std::cout) << std::fixed << std::setprecision(2) << x << "\n";
// The temporary lives to the end of the full expression, so the stream's format is restored at the ';'.
class FormatScope {
public:
    explicit FormatScope(std::ostream& os) : guard_(os), os_(os) {}
    template <class T> std::ostream& operator<<(const T& v) { return os_ << v; }
    std::ostream& operator<<(std::ostream& (*m)(std::ostream&)) { return os_ << m; }
    std::ostream& operator<<(std::ios_base& (*m)(std::ios_base&)) { return os_ << m; }
private:
    StreamFormatGuard guard_;
    std::ostream& os_;
};
inline FormatScope fmt_scoped(std::ostream& os) { return FormatScope(os); }

}  // namespace pimid
