int8_t escape(int8_t v) const { return v; }
int16_t escape(int16_t v) const { return v; }
int32_t escape(int32_t v) const { return v; }
int64_t escape(int64_t v) const { return v; }
uint8_t escape(uint8_t v) const { return v; }
uint16_t escape(uint16_t v) const { return v; }
uint32_t escape(uint32_t v) const { return v; }
uint64_t escape(uint64_t v) const { return v; }
float escape(float v) const { return v; }
double escape(double v) const { return v; }
long double escape(long double v) const { return v; }
bool escape(bool v) const { return v; }
std::string escape(const std::string &v) const { return escape_string(v); }
std::string escape(std::string_view v) const { return escape_string(v); }
std::string escape(const char*v) const { return escape_string(v); }
template<size_t N> std::string escape(const s90::util::varstr<N>& v) const { return escape_string(v); }
template<std::integral T> const std::set<T>& escape(const std::set<T>& v) const { return v; }