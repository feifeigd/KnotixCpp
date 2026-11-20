
#include <cassert>
#include <charconv>
#include <initializer_list>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
// #include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <system_error>

class JsonValue {
public:
    using ValueType = std::variant<
        std::nullptr_t,
        bool,
        double,
        std::string,
        std::vector<JsonValue>,
        std::map<std::string, JsonValue>
    >;
private:
    ValueType value_{nullptr};

    static void appendJsonString(std::ostringstream& out, std::string const& text) {
        constexpr char hex[] = "0123456789abcdef";
        out << '"';
        for (auto ch : text) {
            auto c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (c < 0x20) {
                        out << "\\u00" << hex[c >> 4] << hex[c & 0x0f];
                    } else {
                        out << static_cast<char>(c);
                    }
            }
        }
        out << '"';
    }

public:
    JsonValue() = default;
    JsonValue(JsonValue const&) = default;
    JsonValue(JsonValue&&) noexcept = default;
    JsonValue& operator=(JsonValue const&) = default;
    JsonValue& operator=(JsonValue&&) noexcept = default;

    JsonValue(std::nullptr_t): value_{nullptr}{}
    JsonValue(bool b): value_{b}{}
    JsonValue(double d): value_{d}{}
    JsonValue(int i): value_{static_cast<double>(i)}{}
    JsonValue(std::string const& s): value_{s}{}
    JsonValue(std::string&& s): value_{std::move(s)}{}
    JsonValue(char const* s): value_{s}{}
    // JsonValue(std::string_view sv): value_{std::string{sv.data(), sv.size()}}{}
    JsonValue(std::vector<JsonValue> const& v): value_{v}{}
    JsonValue(std::vector<JsonValue>&& v): value_{std::move(v)}{}
    JsonValue(std::map<std::string, JsonValue> const& m): value_{m}{}
    JsonValue(std::map<std::string, JsonValue>&& m): value_{std::move(m)}{}
    
    template<typename T>
    JsonValue(std::vector<T> const& values){
        std::vector<JsonValue> v;
        for(auto const& value : values)
            v.emplace_back(value);
        value_ = std::move(v);
    }

    static JsonValue array(std::initializer_list<JsonValue> values) {
        return JsonValue(std::vector<JsonValue>(values));
    }

    bool isNull()const{return std::holds_alternative<std::nullptr_t>(value_);}
    bool isBool()const{return std::holds_alternative<bool>(value_);}
    bool isNumber()const{return std::holds_alternative<double>(value_);}
    bool isString()const{return std::holds_alternative<std::string>(value_);}
    bool isArray()const{return std::holds_alternative<std::vector<JsonValue>>(value_);}
    bool isObject()const{return std::holds_alternative<std::map<std::string, JsonValue>>(value_);}

    bool asBool()const{return std::get<bool>(value_);}
    double asNumber()const{return std::get<double>(value_);}
    std::string asString()const{return std::get<std::string>(value_);}
    std::vector<JsonValue> const& asArray()const{return std::get<std::vector<JsonValue>>(value_);}
    std::map<std::string, JsonValue> const& asObject()const{return std::get<std::map<std::string, JsonValue>>(value_);}

    JsonValue const& operator[](std::string const& key)const{
        static JsonValue nullValue;

        if(isObject()){
            auto& obj = asObject();
            if(auto it = obj.find(key); it != obj.end())
                return it->second;
        }

        return nullValue;
    }

    bool hasKey(std::string const& key)const{
        return isObject() && asObject().count(key) > 0;
    }

    std::string toString()const{
        if(isNull())return "null";

        if(isBool()) return asBool() ? "true" : "false";

        std::ostringstream oss;

        if(isNumber()){
            auto d = asNumber();
            if(std::floor(d) == d){
                oss << static_cast<long long>(d);
            }else{
                oss << d;
            }
        }else if(isString()){
            appendJsonString(oss, asString());
        }else if(isArray()){
            oss << "[";
            auto const& arr = asArray();
            for(auto i = 0; i < arr.size(); ++i){
                if(i > 0)oss << ",";
                oss << arr[i].toString();
            }
            oss << "]";
        }else { // object
            oss << "{";
            auto first = true;
            for(auto const& [k, v] : asObject()){
                if(!first)oss << ",";
                first = false;
                appendJsonString(oss, k);
                oss << ":" << v.toString();
            }
            oss << "}";
        }

        return oss.str();
    }
};

class JsonParser {
private:
    std::string const& input_;
    size_t pos_ = 0;

    void skipWhitespace(){
        while(pos_ < input_.size() && std::isspace(input_[pos_])) 
            ++pos_;
    }

    char peek(){
        skipWhitespace();
        return pos_ < input_.size() ? input_[pos_] : '\0';
    }

    char consume(){
        skipWhitespace();
        if(pos_ < input_.size()){
            return input_[pos_++];
        }

        throw std::runtime_error("Unexpected end of JSON");
    }

    void expect(char c){
        if (c != consume()){
            throw std::runtime_error(std::string{"Expected '"} + c + "' at position " + std::to_string(pos_));
        }
    }

    std::string parseString(){
        expect('"');
        std::string result;
        auto size = input_.size();
        char c;
        while(pos_ < size && (c = input_[pos_]) != '"'){
            if('\\' == c){ // 转义
                if(pos_ >= size)throw std::runtime_error("Bad escape");

                char esc = input_[++pos_];
                switch(esc){
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                default: result += c; break;
                }
            }else{
                result += c;
            }
            ++pos_;
        }
        expect('"');
        return result;
    }

    JsonValue parseNumber(){
        skipWhitespace();
        auto start = pos_;
        auto const input_data = input_.data();
        auto const first = input_data + pos_;
        auto const last = input_data + input_.size();
        double value = 0;
        auto [ptr, ec] = std::from_chars(first, last, value, std::chars_format::general);
        if (ec != std::errc{} || ptr == first)
            throw std::runtime_error("Invalid number at position " + std::to_string(start));
        pos_ = static_cast<size_t>(ptr - input_data);
        return value;
    }

    JsonValue parseLiteral(){
        skipWhitespace();
        auto const sub4 = input_.substr(pos_, 4);
        if("null" == sub4){
            pos_ += 4;
            return {};
        }
        if("true" == sub4){
            pos_ += 4;
            return true;
        }
        auto const sub5 = input_.substr(pos_, 5);
        if("false" == sub5){
            pos_ += 5;
            return false;
        }
        throw std::runtime_error("Invalid literal at position: " + std::to_string(pos_));
    }
    
public:
    explicit JsonParser(std::string const& input): input_{input}{}

    JsonValue parse(){
        return parseValue();
    }

    JsonValue parseValue(){
        skipWhitespace();
        if(pos_ >= input_.size())throw std::runtime_error("Empty JSON");

        
        switch(auto c = peek(); c){
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return parseString();
            case 't': case 'f': case 'n': return parseLiteral();
        }
        return parseNumber();
    }

    JsonValue parseObject(){
        expect('{');
        std::map<std::string, JsonValue> obj;
        if(peek() == '}'){
            consume();
            return obj;
        }
        while(true){
            auto key = parseString();
            expect(':');
            obj.emplace(key, parseValue());
            switch(auto c = peek(); c){
                case ',': consume(); continue;
                case '}': consume(); return obj;
                default: throw std::runtime_error("Expected ',' or '}' in object");
            }
        }
    }

    JsonValue parseArray(){
        expect('[');
        std::vector<JsonValue> arr;
        if(peek() == ']'){
            consume();
            return arr;
        }

        while(true){
            arr.push_back(parseValue());
            switch(auto c = peek(); c){
                case ',': consume(); continue;
                case ']': consume(); return arr;
                default: throw std::runtime_error("Expected ',' or ']' in array");
            }
        }
    }
};

template <class Inspector>
bool inspect(Inspector& inspector, JsonValue& value) {
    auto get = [&value] {
        return value.toString();
    };

    auto set = [&value](std::string text) {
        try {
            JsonParser parser{text};
            value = parser.parse();
            return true;
        } catch (std::exception const&) {
            return false;
        }
    };

    return inspector.apply(get, set);
}
