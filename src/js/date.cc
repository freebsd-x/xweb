// Date (21.4): a time value in milliseconds since 1970 UTC, the calendar arithmetic the
// specification spells out, the local time zone through the C library, and a parser
// for the ISO format and the forms pages and other engines write.

#include <time.h>

#include <chrono>
#include <cmath>
#include <cstring>

#include "internal.h"

namespace fbx::xweb::js {

namespace {

using Args = std::span<const Value>;

Value arg(Args a, size_t i) { return i < a.size() ? a[i] : Value::undefined(); }

constexpr double kMsPerDay = 86400000.0;
constexpr double kMaxTime = 8.64e15;

double day(double t) { return std::floor(t / kMsPerDay); }
double timeWithinDay(double t) {
    const double r = std::fmod(t, kMsPerDay);
    return r < 0 ? r + kMsPerDay : r;
}
double daysInYear(double y) {
    if (std::fmod(y, 4) != 0) return 365;
    if (std::fmod(y, 100) != 0) return 366;
    if (std::fmod(y, 400) != 0) return 365;
    return 366;
}
double dayFromYear(double y) {
    return 365 * (y - 1970) + std::floor((y - 1969) / 4) - std::floor((y - 1901) / 100) + std::floor((y - 1601) / 400);
}
double timeFromYear(double y) { return kMsPerDay * dayFromYear(y); }
double yearFromTime(double t) {
    double y = std::floor(t / (kMsPerDay * 365.2425)) + 1970;
    while (timeFromYear(y) > t) --y;
    while (timeFromYear(y + 1) <= t) ++y;
    return y;
}
bool inLeapYear(double t) { return daysInYear(yearFromTime(t)) == 366; }
double dayWithinYear(double t) { return day(t) - dayFromYear(yearFromTime(t)); }

constexpr int kCumulative[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

double monthStart(int m, bool leap) { return kCumulative[m] + (leap && m >= 2 ? 1 : 0); }

double monthFromTime(double t) {
    const double d = dayWithinYear(t);
    const bool leap = inLeapYear(t);
    for (int m = 11; m >= 0; --m)
        if (d >= monthStart(m, leap)) return m;
    return 0;
}
double dateFromTime(double t) {
    const int m = static_cast<int>(monthFromTime(t));
    return dayWithinYear(t) - monthStart(m, inLeapYear(t)) + 1;
}
double weekDay(double t) {
    const double r = std::fmod(day(t) + 4, 7);
    return r < 0 ? r + 7 : r;
}
double hourFromTime(double t) { return std::floor(timeWithinDay(t) / 3600000.0); }
double minFromTime(double t) { return std::fmod(std::floor(timeWithinDay(t) / 60000.0), 60); }
double secFromTime(double t) { return std::fmod(std::floor(timeWithinDay(t) / 1000.0), 60); }
double msFromTime(double t) { return std::fmod(timeWithinDay(t), 1000); }

double toInteger(double d) { return std::isnan(d) ? 0 : std::trunc(d); }

double makeTime(double h, double m, double s, double ms) {
    if (!std::isfinite(h) || !std::isfinite(m) || !std::isfinite(s) || !std::isfinite(ms)) return NAN;
    return toInteger(h) * 3600000.0 + toInteger(m) * 60000.0 + toInteger(s) * 1000.0 + toInteger(ms);
}
double makeDay(double year, double month, double date) {
    if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) return NAN;
    const double y = toInteger(year), m = toInteger(month), dt = toInteger(date);
    const double ym = y + std::floor(m / 12);
    if (std::fabs(ym) > 400000) return NAN;
    const int mn = static_cast<int>(std::fmod(std::fmod(m, 12) + 12, 12));
    const double t = timeFromYear(ym);
    return day(t) + monthStart(mn, daysInYear(ym) == 366) + dt - 1;
}
double makeDate(double d, double t) {
    if (!std::isfinite(d) || !std::isfinite(t)) return NAN;
    const double tv = d * kMsPerDay + t;
    return std::isfinite(tv) ? tv : NAN;
}
double timeClip(double t) {
    if (!std::isfinite(t) || std::fabs(t) > kMaxTime) return NAN;
    return toInteger(t) + 0.0;   // -0 becomes +0
}

// The local zone's offset from UTC at UTC time t, in ms, and its abbreviation.
double offsetAt(double t, std::string* name = nullptr) {
    if (!std::isfinite(t)) return 0;
    // Outside what the C library handles well, the offset of a year with the same calendar.
    double probe = t;
    const double y = yearFromTime(t);
    if (y < 1900 || y > 2200) {
        const double same = 2000 + std::fmod(std::fmod(y - 2000, 28) + 28, 28);
        probe = t - timeFromYear(y) + timeFromYear(same);
    }
    const time_t secs = static_cast<time_t>(std::floor(probe / 1000));
    struct tm tm {};
    if (!localtime_r(&secs, &tm)) return 0;
    if (name && tm.tm_zone) *name = tm.tm_zone;
    return static_cast<double>(tm.tm_gmtoff) * 1000.0;
}
double localTime(double t) { return t + offsetAt(t); }
// A local time read as UTC: the offset that was in force then.
double utcFromLocal(double t) {
    if (!std::isfinite(t)) return NAN;
    const double guess = t - offsetAt(t);
    return t - offsetAt(guess);
}

double now() {
    using namespace std::chrono;
    return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string pad(double v, int width) {
    std::string s = std::to_string(static_cast<long long>(std::fabs(v)));
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), '0');
    return s;
}
std::string yearText(double y) { return (y < 0 ? "-" : "") + pad(y, 4); }

std::string dateText(double t) {
    return std::string(kDays[static_cast<int>(weekDay(t))]) + " " + kMonths[static_cast<int>(monthFromTime(t))] + " " + pad(dateFromTime(t), 2) + " " + yearText(yearFromTime(t));
}
std::string timeText(double t) { return pad(hourFromTime(t), 2) + ":" + pad(minFromTime(t), 2) + ":" + pad(secFromTime(t), 2); }
std::string zoneText(double utc) {
    std::string name;
    const double off = offsetAt(utc, &name) / 60000.0;
    std::string s = std::string("GMT") + (off < 0 ? "-" : "+") + pad(std::floor(std::fabs(off) / 60), 2) + pad(std::fmod(std::fabs(off), 60), 2);
    if (!name.empty()) s += " (" + name + ")";
    return s;
}

std::string isoText(double t) {
    const double y = yearFromTime(t);
    std::string year = (y >= 0 && y <= 9999) ? pad(y, 4) : std::string(y < 0 ? "-" : "+") + pad(y, 6);
    return year + "-" + pad(monthFromTime(t) + 1, 2) + "-" + pad(dateFromTime(t), 2) + "T" + timeText(t) + "." + pad(msFromTime(t), 3) + "Z";
}

// ── Parsing ──

bool digits(const std::string& s, size_t& i, int n, int& out) {
    if (i + n > s.size()) return false;
    out = 0;
    for (int k = 0; k < n; ++k) {
        if (!std::isdigit(static_cast<unsigned char>(s[i + k]))) return false;
        out = out * 10 + (s[i + k] - '0');
    }
    i += n;
    return true;
}

// The ISO form: YYYY[-MM[-DD]][THH:mm[:ss[.sss]]][Z|±HH:mm]; date-only forms are UTC,
// date-times without a zone local.
double parseIso(const std::string& s) {
    size_t i = 0;
    int year = 0, month = 1, dayn = 1, h = 0, mi = 0, sec = 0, ms = 0;
    int sign = 1;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        sign = s[i] == '-' ? -1 : 1;
        ++i;
        if (!digits(s, i, 6, year)) return NAN;
        if (sign < 0 && year == 0) return NAN;
    } else if (!digits(s, i, 4, year)) {
        return NAN;
    }
    year *= sign;
    if (i < s.size() && s[i] == '-') {
        ++i;
        if (!digits(s, i, 2, month) || month < 1 || month > 12) return NAN;
        if (i < s.size() && s[i] == '-') {
            ++i;
            if (!digits(s, i, 2, dayn) || dayn < 1 || dayn > 31) return NAN;
        }
    }
    bool has_time = false, has_zone = false;
    double offset = 0;
    if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
        ++i;
        has_time = true;
        if (!digits(s, i, 2, h) || i >= s.size() || s[i] != ':') return NAN;
        ++i;
        if (!digits(s, i, 2, mi)) return NAN;
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!digits(s, i, 2, sec)) return NAN;
            if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
                ++i;
                size_t start = i;
                int frac = 0, n = 0;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                    if (n < 3) {
                        frac = frac * 10 + (s[i] - '0');
                        ++n;
                    }
                    ++i;
                }
                if (i == start) return NAN;
                while (n < 3) {
                    frac *= 10;
                    ++n;
                }
                ms = frac;
            }
        }
        if (h > 24 || mi > 59 || sec > 59 || (h == 24 && (mi || sec || ms))) return NAN;
    }
    if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) {
        ++i;
        has_zone = true;
    } else if (i < s.size() && (s[i] == '+' || s[i] == '-') && has_time) {
        const int zs = s[i] == '-' ? -1 : 1;
        ++i;
        int zh = 0, zm = 0;
        if (!digits(s, i, 2, zh)) return NAN;
        if (i < s.size() && s[i] == ':') ++i;
        if (!digits(s, i, 2, zm)) return NAN;
        offset = zs * (zh * 60 + zm) * 60000.0;
        has_zone = true;
    }
    if (i != s.size()) return NAN;
    const double t = makeDate(makeDay(year, month - 1, dayn), makeTime(h, mi, sec, ms));
    if (has_zone) return t - offset;
    return has_time ? utcFromLocal(t) : t;
}

// The looser forms: "Tue Nov 08 2026 04:00:00 GMT+0900 (KST)", "Tue, 08 Nov 2026 04:00:00 GMT",
// "November 8, 2026 10:00 PM", "2026/11/08 10:00", "11/8/2026".
double parseLoose(const std::string& in) {
    std::string s;
    // Comments in parentheses go.
    int depth = 0;
    for (char c : in) {
        if (c == '(') ++depth;
        else if (c == ')') depth = depth > 0 ? depth - 1 : 0;
        else if (!depth) s += c;
    }
    int year = -1, month = -1, dayn = -1, h = 0, mi = 0, sec = 0;
    bool has_zone = false, pm = false, am = false;
    double offset = 0;
    std::vector<int> numbers;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isalpha(c)) {
            std::string w;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) w += static_cast<char>(std::tolower(static_cast<unsigned char>(s[i++])));
            if (w == "am" || w == "a") am = true;
            else if (w == "pm" || w == "p") pm = true;
            else if (w == "gmt" || w == "utc" || w == "ut" || w == "z") has_zone = true;
            else if (w == "t") continue;
            else if (w.size() >= 3) {
                bool found = false;
                for (int m = 0; m < 12; ++m) {
                    std::string name = kMonths[m];
                    name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
                    if (w.compare(0, 3, name) == 0) {
                        month = m;
                        found = true;
                    }
                }
                if (!found) {
                    static const char* zones[][2] = {{"est", "-5"}, {"edt", "-4"}, {"cst", "-6"}, {"cdt", "-5"}, {"mst", "-7"}, {"mdt", "-6"}, {"pst", "-8"}, {"pdt", "-7"}};
                    bool zone = false;
                    for (auto& z : zones)
                        if (w == z[0]) {
                            has_zone = true;
                            offset = std::atoi(z[1]) * 3600000.0;
                            zone = true;
                        }
                    bool weekday = false;
                    for (const char* d : kDays) {
                        std::string dn = d;
                        dn[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(dn[0])));
                        if (w.compare(0, 3, dn) == 0) weekday = true;
                    }
                    if (!zone && !weekday) return NAN;
                }
            } else {
                return NAN;
            }
        } else if (std::isdigit(c)) {
            int v = 0;
            size_t start = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) v = v * 10 + (s[i++] - '0');
            if (i < s.size() && s[i] == ':') {
                // A time.
                h = v;
                ++i;
                int n = 0;
                if (!digits(s, i, 2, n)) return NAN;
                mi = n;
                if (i < s.size() && s[i] == ':') {
                    ++i;
                    if (!digits(s, i, 2, n)) return NAN;
                    sec = n;
                    if (i < s.size() && s[i] == '.') {
                        ++i;
                        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                    }
                }
            } else if (i < s.size() && (s[i] == '/' || s[i] == '-') && i - start <= 4) {
                // 2026/11/08, 11/8/2026, 2026-11-8
                int a = v, b = 0, c2 = -1;
                const char sep = s[i++];
                size_t j = i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) b = b * 10 + (s[i++] - '0');
                if (i == j) return NAN;
                if (i < s.size() && s[i] == sep) {
                    ++i;
                    c2 = 0;
                    size_t k = i;
                    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) c2 = c2 * 10 + (s[i++] - '0');
                    if (i == k) return NAN;
                }
                if (start + 4 <= s.size() && i - start >= 4 && (j - 1 - start) == 4) {
                    year = a;
                    month = b - 1;
                    dayn = c2 < 0 ? 1 : c2;
                } else {
                    month = a - 1;
                    dayn = b;
                    if (c2 >= 0) year = c2 < 100 ? (c2 < 50 ? 2000 + c2 : 1900 + c2) : c2;
                }
            } else {
                numbers.push_back(v);
            }
        } else if ((c == '+' || c == '-') && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) && (has_zone || h || mi)) {
            const int zs = c == '-' ? -1 : 1;
            ++i;
            int v = 0, n = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
                v = v * 10 + (s[i++] - '0');
                ++n;
            }
            if (i < s.size() && s[i] == ':') {
                ++i;
                int m2 = 0;
                if (!digits(s, i, 2, m2)) return NAN;
                v = v * 100 + m2;
                n = 4;
            }
            const int hh = n <= 2 ? v : v / 100, mm = n <= 2 ? 0 : v % 100;
            offset = zs * (hh * 60 + mm) * 60000.0;
            has_zone = true;
        } else {
            ++i;
        }
    }
    for (int v : numbers) {
        if (dayn < 0 && v >= 1 && v <= 31 && (year >= 0 || numbers.size() > 1 || month >= 0)) dayn = v;
        else if (year < 0) year = v < 100 ? (v < 50 ? 2000 + v : 1900 + v) : v;
        else if (dayn < 0) dayn = v;
        else return NAN;
    }
    if (year < 0 || month < 0) return NAN;
    if (dayn < 0) dayn = 1;
    if (pm && h < 12) h += 12;
    if (am && h == 12) h = 0;
    const double t = makeDate(makeDay(year, month, dayn), makeTime(h, mi, sec, 0));
    return has_zone ? t - offset : utcFromLocal(t);
}

double parseDate(const std::u16string& text) {
    std::string s = utf8(text);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    s = s.substr(b);
    const double iso = parseIso(s);
    if (!std::isnan(iso)) return timeClip(iso);
    // What looks like the ISO form and isn't valid (a year of -000000) isn't guessed at.
    if (s.size() > 7 && (s[0] == '+' || s[0] == '-') && std::isdigit(static_cast<unsigned char>(s[1]))) return NAN;
    return timeClip(parseLoose(s));
}

Object* thisDate(Runtime& rt, const Value& t, const char* what) {
    if (!t.isObject() || t.asObject()->cls != ObjectClass::Date) rt.throwError("TypeError", std::string("Date.prototype.") + what + " called on something that isn't a Date");
    return t.asObject();
}

double thisTime(Runtime& rt, const Value& t, const char* what) { return thisDate(rt, t, what)->primitive.asNumber(); }

}  // namespace

void Runtime::installDate() {
    Object* proto = newObject();
    Object* ctor = newFunction(u"Date", 7, [proto](Runtime& rt, const Value&, Args a) -> Value {
        Object* nt = rt.construct_target;
        if (!nt) {
            const double t = now();
            const double lt = localTime(t);
            return Value::string(utf16(dateText(lt) + " " + timeText(lt) + " " + zoneText(t)));
        }
        double tv;
        if (a.empty()) {
            tv = now();
        } else if (a.size() == 1) {
            const Value v = a[0];
            if (v.isObject() && v.asObject()->cls == ObjectClass::Date) {
                tv = v.asObject()->primitive.asNumber();
            } else {
                const Value p = rt.toPrimitive(v);
                tv = p.isString() ? parseDate(p.asString()) : rt.toNumber(p);
            }
            tv = timeClip(tv);
        } else {
            double f[7] = {0, 0, 1, 0, 0, 0, 0};
            for (size_t i = 0; i < a.size() && i < 7; ++i) f[i] = rt.toNumber(a[i]);
            if (std::isfinite(f[0])) {
                const double yi = toInteger(f[0]);
                if (yi >= 0 && yi <= 99) f[0] = 1900 + yi;
            }
            tv = timeClip(utcFromLocal(makeDate(makeDay(f[0], f[1], f[2]), makeTime(f[3], f[4], f[5], f[6]))));
        }
        const Value p = rt.get(Value::object(nt), u"prototype");
        Object* d = rt.allocate<Object>(p.isObject() ? p.asObject() : proto, ObjectClass::Date);
        d->primitive = Value::number(tv);
        return Value::object(d);
    }, true);
    put(ctor, u"prototype", Value::object(proto), 0);
    put(proto, u"constructor", Value::object(ctor));
    put(global_, u"Date", Value::object(ctor));

    method(ctor, u"now", 0, [](Runtime&, const Value&, Args) { return Value::number(now()); });
    method(ctor, u"parse", 1, [](Runtime& rt, const Value&, Args a) { return Value::number(parseDate(*rt.toString(arg(a, 0)))); });
    method(ctor, u"UTC", 7, [](Runtime& rt, const Value&, Args a) {
        double f[7] = {NAN, 0, 1, 0, 0, 0, 0};
        for (size_t i = 0; i < a.size() && i < 7; ++i) f[i] = rt.toNumber(a[i]);
        if (std::isfinite(f[0])) {
            const double yi = toInteger(f[0]);
            if (yi >= 0 && yi <= 99) f[0] = 1900 + yi;
        }
        return Value::number(timeClip(makeDate(makeDay(f[0], f[1], f[2]), makeTime(f[3], f[4], f[5], f[6]))));
    });

    // Getters, local and UTC.
    struct Getter {
        const char16_t* name;
        double (*fn)(double);
        bool local;
    };
    static const Getter getters[] = {
        {u"getFullYear", yearFromTime, true}, {u"getUTCFullYear", yearFromTime, false}, {u"getMonth", monthFromTime, true},
        {u"getUTCMonth", monthFromTime, false}, {u"getDate", dateFromTime, true}, {u"getUTCDate", dateFromTime, false},
        {u"getDay", weekDay, true}, {u"getUTCDay", weekDay, false}, {u"getHours", hourFromTime, true}, {u"getUTCHours", hourFromTime, false},
        {u"getMinutes", minFromTime, true}, {u"getUTCMinutes", minFromTime, false}, {u"getSeconds", secFromTime, true},
        {u"getUTCSeconds", secFromTime, false}, {u"getMilliseconds", msFromTime, true}, {u"getUTCMilliseconds", msFromTime, false},
    };
    for (const Getter& g : getters) {
        const Getter* gp = &g;
        method(proto, g.name, 0, [gp](Runtime& rt, const Value& t, Args) {
            const double tv = thisTime(rt, t, "get");
            if (std::isnan(tv)) return Value::number(NAN);
            return Value::number(gp->fn(gp->local ? localTime(tv) : tv));
        });
    }
    method(proto, u"getTime", 0, [](Runtime& rt, const Value& t, Args) { return Value::number(thisTime(rt, t, "getTime")); });
    method(proto, u"valueOf", 0, [](Runtime& rt, const Value& t, Args) { return Value::number(thisTime(rt, t, "valueOf")); });
    method(proto, u"getYear", 0, [](Runtime& rt, const Value& t, Args) {
        const double tv = thisTime(rt, t, "getYear");
        return Value::number(std::isnan(tv) ? NAN : yearFromTime(localTime(tv)) - 1900);
    });
    method(proto, u"getTimezoneOffset", 0, [](Runtime& rt, const Value& t, Args) {
        const double tv = thisTime(rt, t, "getTimezoneOffset");
        return Value::number(std::isnan(tv) ? NAN : (tv - localTime(tv)) / 60000.0);
    });

    // Setters: which field first, how many may follow, local or UTC.
    struct Setter {
        const char16_t* name;
        int field;   // 0 year 1 month 2 date 3 hours 4 minutes 5 seconds 6 ms
        int count;
        bool local;
    };
    static const Setter setters[] = {
        {u"setMilliseconds", 6, 1, true}, {u"setUTCMilliseconds", 6, 1, false}, {u"setSeconds", 5, 2, true}, {u"setUTCSeconds", 5, 2, false},
        {u"setMinutes", 4, 3, true}, {u"setUTCMinutes", 4, 3, false}, {u"setHours", 3, 4, true}, {u"setUTCHours", 3, 4, false},
        {u"setDate", 2, 1, true}, {u"setUTCDate", 2, 1, false}, {u"setMonth", 1, 2, true}, {u"setUTCMonth", 1, 2, false},
        {u"setFullYear", 0, 3, true}, {u"setUTCFullYear", 0, 3, false},
    };
    for (const Setter& st : setters) {
        const Setter* sp = &st;
        method(proto, st.name, st.count, [sp](Runtime& rt, const Value& t, Args a) {
            Object* d = thisDate(rt, t, "set");
            double tv = d->primitive.asNumber();
            double args[4];
            for (int i = 0; i < sp->count; ++i) args[i] = i < static_cast<int>(a.size()) ? rt.toNumber(a[i]) : NAN;
            if (std::isnan(tv)) {
                if (sp->field != 0) return Value::number(NAN);
                tv = 0;   // setFullYear on an invalid date starts from +0
            } else if (sp->local) {
                tv = localTime(tv);
            }
            double f[7] = {yearFromTime(tv), monthFromTime(tv), dateFromTime(tv), hourFromTime(tv), minFromTime(tv), secFromTime(tv), msFromTime(tv)};
            const int fields = sp->field <= 2 ? 3 : 7;
            for (int i = 0; i < sp->count && sp->field + i < fields; ++i)
                if (i < static_cast<int>(a.size()) || i == 0) f[sp->field + i] = args[i];
            double r = makeDate(makeDay(f[0], f[1], f[2]), sp->field <= 2 ? timeWithinDay(tv) : makeTime(f[3], f[4], f[5], f[6]));
            if (sp->local) r = utcFromLocal(r);
            r = timeClip(r);
            d->primitive = Value::number(r);
            return Value::number(r);
        });
    }
    method(proto, u"setTime", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* d = thisDate(rt, t, "setTime");
        const double r = timeClip(rt.toNumber(arg(a, 0)));
        d->primitive = Value::number(r);
        return Value::number(r);
    });
    method(proto, u"setYear", 1, [](Runtime& rt, const Value& t, Args a) {
        Object* d = thisDate(rt, t, "setYear");
        double y = rt.toNumber(arg(a, 0));
        if (std::isnan(y)) {
            d->primitive = Value::number(NAN);
            return Value::number(NAN);
        }
        const double yi = toInteger(y);
        if (yi >= 0 && yi <= 99) y = 1900 + yi;
        const double tv = std::isnan(d->primitive.asNumber()) ? 0 : localTime(d->primitive.asNumber());
        const double r = timeClip(utcFromLocal(makeDate(makeDay(y, monthFromTime(tv), dateFromTime(tv)), timeWithinDay(tv))));
        d->primitive = Value::number(r);
        return Value::number(r);
    });

    // As text.
    const auto text = [&](const char16_t* name, std::string (*fn)(double tv)) {
        method(proto, name, 0, [fn](Runtime& rt, const Value& t, Args) {
            const double tv = thisTime(rt, t, "toString");
            if (std::isnan(tv)) return Value::string(u"Invalid Date");
            return Value::string(utf16(fn(tv)));
        });
    };
    text(u"toString", [](double tv) { const double lt = localTime(tv); return dateText(lt) + " " + timeText(lt) + " " + zoneText(tv); });
    text(u"toDateString", [](double tv) { return dateText(localTime(tv)); });
    text(u"toTimeString", [](double tv) { return timeText(localTime(tv)) + " " + zoneText(tv); });
    text(u"toUTCString", [](double tv) {
        return std::string(kDays[static_cast<int>(weekDay(tv))]) + ", " + pad(dateFromTime(tv), 2) + " " + kMonths[static_cast<int>(monthFromTime(tv))] + " " +
               yearText(yearFromTime(tv)) + " " + timeText(tv) + " GMT";
    });
    put(proto, u"toGMTString", get(Value::object(proto), u"toUTCString"));
    // en-US style, as most pages expect from a browser set to English.
    text(u"toLocaleDateString", [](double tv) {
        const double lt = localTime(tv);
        return std::to_string(static_cast<int>(monthFromTime(lt)) + 1) + "/" + std::to_string(static_cast<int>(dateFromTime(lt))) + "/" + yearText(yearFromTime(lt));
    });
    text(u"toLocaleTimeString", [](double tv) {
        const double lt = localTime(tv);
        const int h = static_cast<int>(hourFromTime(lt));
        return std::to_string(h % 12 == 0 ? 12 : h % 12) + ":" + pad(minFromTime(lt), 2) + ":" + pad(secFromTime(lt), 2) + (h < 12 ? " AM" : " PM");
    });
    text(u"toLocaleString", [](double tv) {
        const double lt = localTime(tv);
        const int h = static_cast<int>(hourFromTime(lt));
        return std::to_string(static_cast<int>(monthFromTime(lt)) + 1) + "/" + std::to_string(static_cast<int>(dateFromTime(lt))) + "/" + yearText(yearFromTime(lt)) + ", " +
               std::to_string(h % 12 == 0 ? 12 : h % 12) + ":" + pad(minFromTime(lt), 2) + ":" + pad(secFromTime(lt), 2) + (h < 12 ? " AM" : " PM");
    });
    method(proto, u"toISOString", 0, [](Runtime& rt, const Value& t, Args) {
        const double tv = thisTime(rt, t, "toISOString");
        if (std::isnan(tv)) rt.throwError("RangeError", "Invalid time value");
        return Value::string(utf16(isoText(tv)));
    });
    method(proto, u"toJSON", 1, [](Runtime& rt, const Value& t, Args) {
        const Value o = Value::object(rt.toObject(t));
        const Value p = rt.toPrimitive(o, "number");
        if (p.isNumber() && !std::isfinite(p.asNumber())) return Value::null();
        return rt.call(rt.get(o, u"toISOString"), o, {});
    });
    {
        Object* f = newFunction(u"[Symbol.toPrimitive]", 1, [](Runtime& rt, const Value& t, Args a) -> Value {
            if (!t.isObject()) rt.throwError("TypeError", "Date.prototype[Symbol.toPrimitive] called on a non-object");
            const Value h = arg(a, 0);
            const std::u16string hint = h.isString() ? h.asString() : u"";
            if (hint != u"string" && hint != u"default" && hint != u"number") rt.throwError("TypeError", "an invalid hint");
            const bool string_first = hint != u"number";
            for (int pass = 0; pass < 2; ++pass) {
                const char16_t* name = (pass == 0) == string_first ? u"toString" : u"valueOf";
                const Value fn = rt.get(t, name);
                if (rt.isCallable(fn)) {
                    const Value r = rt.call(fn, t, {});
                    if (!r.isObject()) return r;
                }
            }
            rt.throwError("TypeError", "can't convert a Date to a primitive value");
        });
        proto->define(PropertyKey(sym_to_primitive), {Value::object(f), nullptr, nullptr, kConfigurable});
    }
}

}  // namespace fbx::xweb::js
