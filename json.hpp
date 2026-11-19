
#include <cassert>
#include <map>
#include <sstream>
#include <string>
// #include <string_view>
#include <variant>
#include <vector>

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
public:
    JsonValue() = default;
    JsonValue(std::nullptr_t): value_{nullptr}{}
    JsonValue(bool b): value_{b}{}
    JsonValue(double d): value_{d}{}
    JsonValue(int i): value_{static_cast<double>(i)}{}
    JsonValue(std::string const& s): value_{s}{}
    JsonValue(char const* s): value_{s}{}
    // JsonValue(std::string_view sv): value_{std::string{sv.data(), sv.size()}}{}
    JsonValue(std::vector<JsonValue> const& v): value_{v}{}
    JsonValue(std::map<std::string, JsonValue> const& m): value_{m}{}
    JsonValue(std::initializer_list<JsonValue> values): value_{std::vector<JsonValue>(values)}{}
    
    template<typename T>
    JsonValue(std::vector<T> const& values){
        std::vector<JsonValue> v;
        for(auto const& value : values)
            v.emplace_back(value);
        value_ = std::move(v);
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
            oss << "\"";
            for(auto c : asString()){
                switch(c){
                case '"': {
                        oss << "\\\"";
                    }
                    break;
                case '\\': {
                        oss << "\\\\";
                    }
                    break;
                case '\n': {
                        oss << "\\n";
                    }
                    break;
                default:
                    oss << c;
                }
            }
            oss << "\"";
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
                oss << "\"" << k << "\":" << v.toString();
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
        auto size = input_.size();
        
        if(pos_ < size ){
            if(auto c = input_[pos_]; '-' == c || '+' == c)
                ++pos_;            
        }
        while(pos_ < size){
            auto c = input_[pos_];
            switch(c){
            case '.': case 'e': case 'E': case '-': case '+': ++pos_;
            default:
                if(std::isdigit(c))
                    ++pos_;
                break;
            }
        }

        return JsonValue{std::stod(input_.substr(start, pos_ - start))};
    }

    JsonValue parseLiteral(){
        skipWhitespace();
        auto sub4 = input_.substr(pos_, 4);
        if("null" == sub4){
            pos_ += 4;
            return {};
        }
        if("true" == sub4){
            pos_ += 4;
            return true;
        }
        auto sub5 = input_.substr(pos_, 5);
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
                case '}': consume(); break;
                default: throw std::runtime_error("Expected ',' or '}' in object");
            }
        }
        return obj;
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
                case ']': consume(); break;
                default: throw std::runtime_error("Expected ',' or ']' in array");
            }
        }

        return arr;
    }
};
