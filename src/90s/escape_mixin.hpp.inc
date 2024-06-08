template<std::integral T> T escape(T v) const { return v; }
float escape(float v) const { return v; }
double escape(double v) const { return v; }
long double escape(long double v) const { return v; }
bool escape(bool v) const { return v; }
std::string escape(const std::string &v) const { return escape_string(v); }
std::string escape(std::string_view v) const { return escape_string(v); }
std::string escape(const char *v) const { return escape_string(v); }
template<size_t N> std::string escape(const s90::orm::varchar<N>& v) const { return escape_string(v); }
template<std::integral T> const std::set<T>& escape(const std::set<T>& v) const { return v; }
std::string escape(const s90::orm::timestamp& v) const { return v.from_native(); }
std::string escape(const s90::orm::datetime& v) const { return v.from_native(); }
