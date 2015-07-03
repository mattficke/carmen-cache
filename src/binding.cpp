
#include "binding.hpp"

#include "pbf.hpp"

#include <sstream>
#include <cmath>
#include <cassert>

namespace carmen {

using namespace v8;

Persistent<FunctionTemplate> Cache::constructor;

inline std::string shard(uint64_t level, uint32_t id) {
    if (level == 0) return "0";
    unsigned int bits = 32 - (static_cast<unsigned int>(level) * 4);
    unsigned int shard_id = static_cast<unsigned int>(std::floor(id / std::pow(2, bits)));
    return std::to_string(shard_id);
}

inline std::vector<unsigned short> arrayToVector(Local<Array> const& array) {
    std::vector<unsigned short> cpp_array;
    cpp_array.reserve(array->Length());
    for (uint32_t i = 0; i < array->Length(); i++) {
        int64_t js_value = array->Get(i)->IntegerValue();
        if (js_value < 0 || js_value >= std::numeric_limits<unsigned short>::max()) {
            std::stringstream s;
            s << "value in array too large (cannot fit '" << js_value << "' in unsigned short)";
            throw std::runtime_error(s.str());
        }
        cpp_array.emplace_back(static_cast<unsigned short>(js_value));
    }
    return cpp_array;
}

inline Local<Array> vectorToArray(Cache::intarray const& vector) {
    std::size_t size = vector.size();
    Local<Array> array = NanNew<Array>(static_cast<int>(size));
    for (uint32_t i = 0; i < size; i++) {
        array->Set(i, NanNew<Number>(vector[i]));
    }
    return array;
}

inline Local<Object> mapToObject(std::map<std::uint64_t,std::uint64_t> const& map) {
    Local<Object> object = NanNew<Object>();
    for (auto const& item : map) {
        object->Set(NanNew<Number>(item.first), NanNew<Number>(item.second));
    }
    return object;
}

inline std::map<std::uint64_t,std::uint64_t> objectToMap(Local<Object> const& object) {
    std::map<std::uint64_t,std::uint64_t> map;
    const Local<Array> keys = object->GetPropertyNames();
    const uint32_t length = keys->Length();
    for (uint32_t i = 0; i < length; i++) {
        uint32_t key = static_cast<uint32_t>(keys->Get(i)->IntegerValue());
        uint64_t value = static_cast<uint64_t>(object->Get(key)->NumberValue());
        map.emplace(key, value);
    }
    return map;
}

Cache::intarray __get(Cache const* c, std::string const& type, std::string const& shard, uint32_t id) {
    std::string key = type + "-" + shard;
    Cache::memcache const& mem = c->cache_;
    Cache::memcache::const_iterator itr = mem.find(key);
    Cache::intarray array;
    if (itr == mem.end()) {
        Cache::lazycache const& lazy = c->lazy_;
        Cache::lazycache::const_iterator litr = lazy.find(key);
        if (litr == lazy.end()) {
            return array;
        }
        Cache::message_cache const& messages = c->msg_;
        Cache::message_cache::const_iterator mitr = messages.find(key);
        if (mitr == messages.end()) {
            throw std::runtime_error("misuse");
        }
        Cache::larraycache::const_iterator laitr = litr->second.find(id);
        if (laitr == litr->second.end()) {
            return array;
        } else {
            // NOTE: we cannot call array.reserve here since
            // the total length is not known
            unsigned start = (laitr->second & 0xffffffff);
            unsigned len = (laitr->second >> 32);
            std::string ref = mitr->second.substr(start,len);
            protobuf::message buffer(ref.data(), ref.size());
            while (buffer.next()) {
                if (buffer.tag == 1) {
                    buffer.skip();
                } else if (buffer.tag == 2) {
                    uint64_t array_length = buffer.varint();
                    protobuf::message pbfarray(buffer.getData(),static_cast<std::size_t>(array_length));
                    while (pbfarray.next()) {
                        array.emplace_back(pbfarray.value);
                    }
                    buffer.skipBytes(array_length);
                } else {
                    std::stringstream msg("");
                    msg << "cxx get: hit unknown protobuf type: '" << buffer.tag << "'";
                    throw std::runtime_error(msg.str());
                }
            }
            return array;
        }
    } else {
        Cache::arraycache::const_iterator aitr = itr->second.find(id);
        if (aitr == itr->second.end()) {
            return array;
        } else {
            return aitr->second;
        }
    }
}

bool __exists(Cache const* c, std::string const& type, std::string const& shard, uint32_t id) {
    std::string key = type + "-" + shard;
    Cache::memcache const& mem = c->cache_;
    Cache::memcache::const_iterator itr = mem.find(key);
    if (itr == mem.end()) {
        Cache::lazycache const& lazy = c->lazy_;
        Cache::lazycache::const_iterator litr = lazy.find(key);
        if (litr == lazy.end()) {
            return false;
        }
        Cache::message_cache const& messages = c->msg_;
        Cache::message_cache::const_iterator mitr = messages.find(key);
        if (mitr == messages.end()) {
            throw std::runtime_error("misuse");
        }
        Cache::larraycache::const_iterator laitr = litr->second.find(id);
        if (laitr == litr->second.end()) {
            return false;
        }
        // NOTE: we cannot call array.reserve here since
        // the total length is not known
        unsigned start = (laitr->second & 0xffffffff);
        unsigned len = (laitr->second >> 32);
        std::string ref = mitr->second.substr(start,len);
        protobuf::message buffer(ref.data(), ref.size());
        while (buffer.next()) {
            if (buffer.tag == 1) {
                buffer.skip();
            } else if (buffer.tag == 2) {
                return true;
            } else {
                std::stringstream msg("");
                msg << "cxx get: hit unknown protobuf type: '" << buffer.tag << "'";
                throw std::runtime_error(msg.str());
            }
        }
        return false;
    } else {
        Cache::arraycache::const_iterator aitr = itr->second.find(id);
        return aitr != itr->second.end();
    }
}

void Cache::Initialize(Handle<Object> target) {
    NanScope();
    Local<FunctionTemplate> t = NanNew<FunctionTemplate>(Cache::New);
    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(NanNew("Cache"));
    NODE_SET_PROTOTYPE_METHOD(t, "has", has);
    NODE_SET_PROTOTYPE_METHOD(t, "load", load);
    NODE_SET_PROTOTYPE_METHOD(t, "loadSync", loadSync);
    NODE_SET_PROTOTYPE_METHOD(t, "pack", pack);
    NODE_SET_PROTOTYPE_METHOD(t, "list", list);
    NODE_SET_PROTOTYPE_METHOD(t, "_set", _set);
    NODE_SET_PROTOTYPE_METHOD(t, "_get", _get);
    NODE_SET_PROTOTYPE_METHOD(t, "_exists", _exists);
    NODE_SET_PROTOTYPE_METHOD(t, "unload", unload);
    NODE_SET_METHOD(t, "coalesce", coalesce);
    target->Set(NanNew("Cache"),t->GetFunction());
    NanAssignPersistent(constructor, t);
}

Cache::Cache()
  : ObjectWrap(),
    cache_(),
    lazy_(),
    msg_() {}

Cache::~Cache() { }

NAN_METHOD(Cache::pack)
{
    NanScope();
    if (args.Length() < 1) {
        return NanThrowTypeError("expected two args: 'type', 'shard'");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first argument must be a String");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an Integer");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        Cache::memcache const& mem = c->cache_;
        Cache::memcache::const_iterator itr = mem.find(key);
        carmen::proto::object message;
        if (itr != mem.end()) {
            for (auto const& item : itr->second) {
                ::carmen::proto::object_item * new_item = message.add_items(); 
                new_item->set_key(item.first);
                Cache::intarray const & varr = item.second;
                for (auto const& vitem : varr) {
                    new_item->add_val(static_cast<int64_t>(vitem));
                }
            }
        } else {
            Cache::lazycache const& lazy = c->lazy_;
            Cache::message_cache const& messages = c->msg_;
            Cache::lazycache::const_iterator litr = lazy.find(key);
            if (litr != lazy.end()) {
                Cache::message_cache::const_iterator mitr = messages.find(key);
                if (mitr == messages.end()) {
                    throw std::runtime_error("misuse");
                }
                for (auto const& item : litr->second) {
                    ::carmen::proto::object_item * new_item = message.add_items();
                    new_item->set_key(static_cast<int64_t>(item.first));
                    unsigned start = (item.second & 0xffffffff);
                    unsigned len = (item.second >> 32);
                    std::string ref = mitr->second.substr(start,len);
                    protobuf::message buffer(ref.data(), ref.size());
                    while (buffer.next()) {
                        if (buffer.tag == 1) {
                            buffer.skip();
                        } else if (buffer.tag == 2) {
                            uint64_t array_length = buffer.varint();
                            protobuf::message pbfarray(buffer.getData(),static_cast<std::size_t>(array_length));
                            while (pbfarray.next()) {
                                new_item->add_val(static_cast<int64_t>(pbfarray.value));
                            }
                            buffer.skipBytes(array_length);
                        } else {
                            std::stringstream msg("");
                            msg << "pack: hit unknown protobuf type: '" << buffer.tag << "'";
                            throw std::runtime_error(msg.str());
                        }
                    }
                }
            } else {
                return NanThrowTypeError("pack: cannot pack empty data");
            }
        }
        int size = message.ByteSize();
        if (size > 0)
        {
            uint32_t usize = static_cast<uint32_t>(size);
            Local<Object> buf = NanNewBufferHandle(usize);
            if (message.SerializeToArray(node::Buffer::Data(buf),size))
            {
                NanReturnValue(buf);
            }
        } else {
            return NanThrowTypeError("pack: invalid message ByteSize encountered");
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnUndefined();
}

NAN_METHOD(Cache::list)
{
    NanScope();
    if (args.Length() < 1) {
        return NanThrowTypeError("expected at least one arg: 'type' and optional a 'shard'");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first argument must be a String");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        Cache::memcache const& mem = c->cache_;
        Cache::lazycache const& lazy = c->lazy_;
        Local<Array> ids = NanNew<Array>();
        if (args.Length() == 1) {
            unsigned idx = 0;
            std::size_t type_size = type.size();
            for (auto const& item : mem) {
                std::size_t item_size = item.first.size();
                if (item_size > type_size && item.first.substr(0,type_size) == type) {
                    std::string shard = item.first.substr(type_size+1,item_size);
                    ids->Set(idx++,NanNew(NanNew(shard.c_str())->NumberValue()));
                }
            }
            for (auto const& item : lazy) {
                std::size_t item_size = item.first.size();
                if (item_size > type_size && item.first.substr(0,type_size) == type) {
                    std::string shard = item.first.substr(type_size+1,item_size);
                    ids->Set(idx++,NanNew(NanNew(shard.c_str())->NumberValue()));
                }
            }
            NanReturnValue(ids);
        } else if (args.Length() == 2) {
            std::string shard = *String::Utf8Value(args[1]->ToString());
            std::string key = type + "-" + shard;
            Cache::memcache::const_iterator itr = mem.find(key);
            unsigned idx = 0;
            if (itr != mem.end()) {
                for (auto const& item : itr->second) {
                    ids->Set(idx++,NanNew(item.first)->ToString());
                }
            }
            Cache::lazycache::const_iterator litr = lazy.find(key);
            if (litr != lazy.end()) {
                for (auto const& item : litr->second) {
                    ids->Set(idx++,NanNew<Number>(item.first)->ToString());
                }
            }
            NanReturnValue(ids);
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnUndefined();
}

NAN_METHOD(Cache::_set)
{
    NanScope();
    if (args.Length() < 3) {
        return NanThrowTypeError("expected four args: 'type', 'shard', 'id', 'data'");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first argument must be a String");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an Integer");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg must be an Integer");
    }
    if (!args[3]->IsArray()) {
        return NanThrowTypeError("fourth arg must be an Array");
    }
    Local<Array> data = Local<Array>::Cast(args[3]);
    if (data->IsNull() || data->IsUndefined()) {
        return NanThrowTypeError("an array expected for fourth argument");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        Cache::memcache & mem = c->cache_;
        Cache::memcache::const_iterator itr = mem.find(key);
        if (itr == mem.end()) {
            c->cache_.emplace(key,Cache::arraycache());
        }
        Cache::arraycache & arrc = c->cache_[key];
        Cache::arraycache::key_type key_id = static_cast<Cache::arraycache::key_type>(args[2]->IntegerValue());
        Cache::arraycache::iterator itr2 = arrc.find(key_id);
        if (itr2 == arrc.end()) {
            arrc.emplace(key_id,Cache::intarray());
        }
        Cache::intarray & vv = arrc[key_id];

        unsigned array_size = data->Length();
        if (args[4]->IsBoolean() && args[4]->BooleanValue()) {
            vv.reserve(vv.size() + array_size);
        } else {
            if (itr2 != arrc.end()) vv.clear();
            vv.reserve(array_size);
        }

        for (unsigned i=0;i<array_size;++i) {
            vv.emplace_back(static_cast<uint64_t>(data->Get(i)->NumberValue()));
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnUndefined();
}

void load_into_cache(Cache::larraycache & larrc,
                            const char * data,
                            size_t size) {
    protobuf::message message(data,size);
    while (message.next()) {
        if (message.tag == 1) {
            uint64_t len = message.varint();
            protobuf::message buffer(message.getData(), static_cast<std::size_t>(len));
            while (buffer.next()) {
                if (buffer.tag == 1) {
                    uint64_t key_id = buffer.varint();
                    size_t start = static_cast<size_t>(message.getData() - data);
                    // insert here because:
                    //  - libstdc++ does not support std::map::emplace <-- does now with gcc 4.8, but...
                    //  - we are using google::sparshash now, which does not support emplace (yet?)
                    //  - larrc.emplace(buffer.varint(),Cache::string_ref_type(message.getData(),len)) was not faster on OS X
                    Cache::offset_type offsets = (((Cache::offset_type)len << 32)) | (((Cache::offset_type)start) & 0xffffffff);
                    larrc.insert(std::make_pair(key_id,offsets));
                }
                // it is safe to break immediately because tag 1 should come first
                // it would also be safe to not use `while (buffer.next())` here, but we do it
                // because I've not seen a performance cost (dane/osx) and being explicit is good
                break;
            }
            message.skipBytes(len);
        } else {
            std::stringstream msg("");
            msg << "load: hit unknown protobuf type: '" << message.tag << "'";
            throw std::runtime_error(msg.str());
        }
    }
}

NAN_METHOD(Cache::loadSync)
{
    NanScope();
    if (args.Length() < 2) {
        return NanThrowTypeError("expected at three args: 'buffer', 'type', and 'shard'");
    }
    if (!args[0]->IsObject()) {
        return NanThrowTypeError("first argument must be a Buffer");
    }
    Local<Object> obj = args[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined()) {
        return NanThrowTypeError("a buffer expected for first argument");
    }
    if (!node::Buffer::HasInstance(obj)) {
        return NanThrowTypeError("first argument must be a Buffer");
    }
    if (!args[1]->IsString()) {
        return NanThrowTypeError("second arg 'type' must be a String");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg 'shard' must be an Integer");
    }
    try {
        std::string type = *String::Utf8Value(args[1]->ToString());
        std::string shard = *String::Utf8Value(args[2]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        Cache::memcache & mem = c->cache_;
        Cache::memcache::iterator itr = mem.find(key);
        if (itr != mem.end()) {
            c->cache_.emplace(key,arraycache());
        }
        Cache::memcache::iterator itr2 = mem.find(key);
        if (itr2 != mem.end()) {
            mem.erase(itr2);
        }
        Cache::lazycache & lazy = c->lazy_;
        Cache::lazycache::iterator litr = lazy.find(key);
        Cache::message_cache & messages = c->msg_;
        if (litr == lazy.end()) {
            c->lazy_.emplace(key,Cache::larraycache());
            messages.emplace(key,std::string(node::Buffer::Data(obj),node::Buffer::Length(obj)));
        }
        load_into_cache(c->lazy_[key],node::Buffer::Data(obj),node::Buffer::Length(obj));
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnUndefined();
}

struct load_baton : carmen::noncopyable {
    uv_work_t request;
    Cache * c;
    NanCallback cb;
    Cache::larraycache arrc;
    std::string key;
    std::string data;
    std::string error_name;
    load_baton(std::string const& _key,
               const char * _data,
               size_t size,
               Local<Function> callbackHandle,
               Cache * _c) :
      c(_c),
      cb(callbackHandle),
      arrc(),
      key(_key),
      data(_data,size),
      error_name() {
        request.data = this;
        c->_ref();
      }
    ~load_baton() {
         c->_unref();
    }
};

void Cache::AsyncLoad(uv_work_t* req) {
    load_baton *closure = static_cast<load_baton *>(req->data);
    try {
        load_into_cache(closure->arrc,closure->data.data(),closure->data.size());
    }
    catch (std::exception const& ex)
    {
        closure->error_name = ex.what();
    }
}

void Cache::AfterLoad(uv_work_t* req) {
    NanScope();
    load_baton *closure = static_cast<load_baton *>(req->data);
    TryCatch try_catch;
    if (!closure->error_name.empty()) {
        Local<Value> argv[1] = { Exception::Error(NanNew(closure->error_name.c_str())) };
        closure->cb.Call(1, argv);
    } else {
        Cache::memcache::iterator itr2 = closure->c->cache_.find(closure->key);
        if (itr2 != closure->c->cache_.end()) {
            closure->c->cache_.erase(itr2);
        }
        closure->c->lazy_[closure->key] = std::move(closure->arrc);
        Local<Value> argv[1] = { NanNull() };
        closure->cb.Call(1, argv);
    }
    if (try_catch.HasCaught())
    {
        node::FatalException(try_catch);
    }
    delete closure;
}

NAN_METHOD(Cache::load)
{
    NanScope();
    Local<Value> callback = args[args.Length()-1];
    if (!callback->IsFunction()) {
        return loadSync(args);
    }
    if (args.Length() < 2) {
        return NanThrowTypeError("expected at least three args: 'buffer', 'type', 'shard', and optionally a 'callback'");
    }
    if (!args[0]->IsObject()) {
        return NanThrowTypeError("first argument must be a Buffer");
    }
    Local<Object> obj = args[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined()) {
        return NanThrowTypeError("a buffer expected for first argument");
    }
    if (!node::Buffer::HasInstance(obj)) {
        return NanThrowTypeError("first argument must be a Buffer");
    }
    if (!args[1]->IsString()) {
        return NanThrowTypeError("second arg 'type' must be a String");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg 'shard' must be an Integer");
    }
    try {
        std::string type = *String::Utf8Value(args[1]->ToString());
        std::string shard = *String::Utf8Value(args[2]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        load_baton *closure = new load_baton(key,
                                             node::Buffer::Data(obj),
                                             node::Buffer::Length(obj),
                                             args[3].As<Function>(),
                                             c);
        uv_queue_work(uv_default_loop(), &closure->request, AsyncLoad, (uv_after_work_cb)AfterLoad);
        Cache::message_cache & messages = c->msg_;
        if (messages.find(key) == messages.end()) {
            messages.emplace(key,std::string(node::Buffer::Data(obj),node::Buffer::Length(obj)));
        }
        NanReturnUndefined();
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
}

NAN_METHOD(Cache::has)
{
    NanScope();
    if (args.Length() < 2) {
        return NanThrowTypeError("expected two args: 'type' and 'shard'");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first arg must be a String");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an Integer");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        Cache::memcache const& mem = c->cache_;
        Cache::memcache::const_iterator itr = mem.find(key);
        if (itr != mem.end()) {
            NanReturnValue(NanTrue());
        } else {
            Cache::message_cache const& messages = c->msg_;
            if (messages.find(key) != messages.end()) {
                NanReturnValue(NanTrue());
            }
            NanReturnValue(NanFalse());
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
}

NAN_METHOD(Cache::_get)
{
    NanScope();
    if (args.Length() < 3) {
        return NanThrowTypeError("expected three args: type, shard, and id");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first arg must be a String");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an Integer");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg must be a positive Integer");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        int64_t id = args[2]->IntegerValue();
        if (id < 0) {
            return NanThrowTypeError("third arg must be a positive Integer");
        }
        if (id > std::numeric_limits<uint32_t>::max()) {
            return NanThrowTypeError("third arg must be a positive Integer that fits within 32 bits");
        }
        uint32_t id2 = static_cast<uint32_t>(id);
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        Cache::intarray vector = __get(c, type, shard, id2);
        if (!vector.empty()) {
            NanReturnValue(vectorToArray(vector));
        } else {
            NanReturnUndefined();
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
}

NAN_METHOD(Cache::_exists)
{
    NanScope();
    if (args.Length() < 3) {
        return NanThrowTypeError("expected three args: type, shard, and id");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first arg must be a String");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an Integer");
    }
    if (!args[2]->IsNumber()) {
        return NanThrowTypeError("third arg must be an Integer");
    }
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        uint32_t id = static_cast<uint32_t>(args[2]->IntegerValue());
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        bool exists = __exists(c, type, shard, id);
        NanReturnValue(NanNew<Boolean>(exists));
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
}

NAN_METHOD(Cache::unload)
{
    NanScope();
    if (args.Length() < 2) {
        return NanThrowTypeError("expected at least two args: 'type' and 'shard'");
    }
    if (!args[0]->IsString()) {
        return NanThrowTypeError("first arg must be a String");
    }
    if (!args[1]->IsNumber()) {
        return NanThrowTypeError("second arg must be an Integer");
    }
    bool hit = false;
    try {
        std::string type = *String::Utf8Value(args[0]->ToString());
        std::string shard = *String::Utf8Value(args[1]->ToString());
        std::string key = type + "-" + shard;
        Cache* c = node::ObjectWrap::Unwrap<Cache>(args.This());
        Cache::memcache & mem = c->cache_;
        Cache::memcache::iterator itr = mem.find(key);
        if (itr != mem.end()) {
            hit = true;
            mem.erase(itr);
        }
        Cache::lazycache & lazy = c->lazy_;
        Cache::lazycache::iterator litr = lazy.find(key);
        if (litr != lazy.end()) {
            hit = true;
            lazy.erase(litr);
        }
        Cache::message_cache & messages = c->msg_;
        Cache::message_cache::iterator mitr = messages.find(key);
        if (mitr != messages.end()) {
            hit = true;
            messages.erase(mitr);
        }
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnValue(NanNew<Boolean>(hit));
}

NAN_METHOD(Cache::New)
{
    NanScope();
    if (!args.IsConstructCall()) {
        return NanThrowTypeError("Cannot call constructor as function, you need to use 'new' keyword");
    }
    try {
        if (args.Length() < 2) {
            return NanThrowTypeError("expected 'id' and 'shardlevel' arguments");
        }
        if (!args[0]->IsString()) {
            return NanThrowTypeError("first argument 'id' must be a String");
        }
        if (!args[1]->IsNumber()) {
            return NanThrowTypeError("second argument 'shardlevel' must be a number");
        }
        Cache* im = new Cache();
        im->Wrap(args.This());
        args.This()->Set(NanNew("id"),args[0]);
        args.This()->Set(NanNew("shardlevel"),args[1]);
        NanReturnValue(args.This());
    } catch (std::exception const& ex) {
        return NanThrowTypeError(ex.what());
    }
    NanReturnUndefined();
}

//relev = 5 bits
//count = 3 bits
//reason = 12 bits
//* 1 bit gap
//id = 32 bits
constexpr double _pow(double x, int y)
{
    return y == 0 ? 1.0 : x * _pow(x, y-1);
}

constexpr uint64_t POW2_52 = static_cast<uint64_t>(_pow(2.0,52));
constexpr uint64_t POW2_48 = static_cast<uint64_t>(_pow(2.0,48));
constexpr uint64_t POW2_45 = static_cast<uint64_t>(_pow(2.0,45));
constexpr uint64_t POW2_33 = static_cast<uint64_t>(_pow(2.0,33));
constexpr uint64_t POW2_32 = static_cast<uint64_t>(_pow(2.0,32));
constexpr uint64_t POW2_28 = static_cast<uint64_t>(_pow(2.0,28));
constexpr uint64_t POW2_25 = static_cast<uint64_t>(_pow(2.0,25));
constexpr uint64_t POW2_20 = static_cast<uint64_t>(_pow(2.0,20));
constexpr uint64_t POW2_14 = static_cast<uint64_t>(_pow(2.0,14));
constexpr uint64_t POW2_12 = static_cast<uint64_t>(_pow(2.0,12));
constexpr uint64_t POW2_8 = static_cast<uint64_t>(_pow(2.0,8));
constexpr uint64_t POW2_5 = static_cast<uint64_t>(_pow(2.0,5));
constexpr uint64_t POW2_4 = static_cast<uint64_t>(_pow(2.0,4));
constexpr uint64_t POW2_3 = static_cast<uint64_t>(_pow(2.0,3));
constexpr uint64_t POW2_2 = static_cast<uint64_t>(_pow(2.0,2));

struct PhrasematchSubq {
    carmen::Cache *cache;
    double weight;
    uint64_t shardlevel;
    uint32_t phrase;
    unsigned short idx;
    unsigned short zoom;
};

struct Cover {
    double relev;
    uint32_t id;
    uint32_t tmpid;
    unsigned short x;
    unsigned short y;
    unsigned short score;
    unsigned short idx;
    unsigned short distance;
};

struct Context {
    std::vector<Cover> coverList;
    double relev;
};

Cover numToCover(uint64_t num) {
    Cover cover;
    assert(((num >> 39) % POW2_14) <= static_cast<double>(std::numeric_limits<unsigned short>::max()));
    assert(((num >> 39) % POW2_14) >= static_cast<double>(std::numeric_limits<unsigned short>::min()));
    unsigned short x = static_cast<unsigned short>((num >> 39) % POW2_14);
    assert(((num >> 25) % POW2_14) <= static_cast<double>(std::numeric_limits<unsigned short>::max()));
    assert(((num >> 25) % POW2_14) >= static_cast<double>(std::numeric_limits<unsigned short>::min()));
    unsigned short y = static_cast<unsigned short>((num >> 25) % POW2_14);
    assert(((num >> 20) % POW2_3) <= static_cast<double>(std::numeric_limits<unsigned short>::max()));
    assert(((num >> 20) % POW2_3) >= static_cast<double>(std::numeric_limits<unsigned short>::min()));
    unsigned short score = static_cast<unsigned short>((num >> 20) % POW2_3);
    uint32_t id = static_cast<uint32_t>(num % POW2_20);
    cover.x = x;
    cover.y = y;
    double relev = 0.4 + (0.2 * ((num >> 23) % POW2_2));
    cover.relev = relev;
    cover.score = score;
    cover.id = id;

    // These are not derived from decoding the input num but by
    // external values after initialization.
    cover.idx = 0;
    cover.tmpid = 0;
    cover.distance = 0;

    return cover;
};

struct ZXY {
    unsigned short z;
    unsigned short x;
    unsigned short y;
};

ZXY pxy2zxy(unsigned short z, unsigned short x, unsigned short y, unsigned short target_z) {
    ZXY zxy;
    zxy.z = target_z;

    // Interval between parent and target zoom level
    unsigned short zDist = target_z - z;
    unsigned short zMult = zDist - 1;
    if (zDist == 0) {
        zxy.x = x;
        zxy.y = y;
        return zxy;
    }

    // Midpoint length @ z for a tile at parent zoom level
    double pMid_d = std::pow(2,zDist) / 2.0;
    assert(pMid_d <= static_cast<double>(std::numeric_limits<unsigned short>::max()));
    assert(pMid_d >= static_cast<double>(std::numeric_limits<unsigned short>::min()));
    unsigned short pMid = static_cast<unsigned short>(pMid_d);
    zxy.x = (x * zMult) + pMid;
    zxy.y = (y * zMult) + pMid;
    return zxy;
}

inline bool coverSortByRelev(Cover const& a, Cover const& b) noexcept {
    if (b.relev > a.relev) return false;
    else if (b.relev < a.relev) return true;
    else if (b.score > a.score) return false;
    else if (b.score < a.score) return true;
    else if (b.idx < a.idx) return false;
    else if (b.idx > a.idx) return true;
    return (b.id > a.id);
}

inline bool coverSortByRelevDistance(Cover const& a, Cover const& b) noexcept {
    if (b.relev > a.relev) return false;
    else if (b.relev < a.relev) return true;
    else if (b.distance < a.distance) return false;
    else if (b.distance > a.distance) return true;
    else if (b.score > a.score) return false;
    else if (b.score < a.score) return true;
    else if (b.idx < a.idx) return false;
    else if (b.idx > a.idx) return true;
    return (b.id > a.id);
}

inline bool contextSortByRelev(Context const& a, Context const& b) noexcept {
    if (b.relev > a.relev) return false;
    else if (b.relev < a.relev) return true;
    else if (b.coverList[0].score > a.coverList[0].score) return false;
    else if (b.coverList[0].score < a.coverList[0].score) return true;
    else if (b.coverList[0].idx < a.coverList[0].idx) return false;
    else if (b.coverList[0].idx > a.coverList[0].idx) return true;
    return (b.coverList[0].id > a.coverList[0].id);
}

inline bool contextSortByRelevDistance(Context const& a, Context const& b) noexcept {
    if (b.relev > a.relev) return false;
    else if (b.relev < a.relev) return true;
    else if (b.coverList[0].distance < a.coverList[0].distance) return false;
    else if (b.coverList[0].distance > a.coverList[0].distance) return true;
    else if (b.coverList[0].score > a.coverList[0].score) return false;
    else if (b.coverList[0].score < a.coverList[0].score) return true;
    else if (b.coverList[0].idx < a.coverList[0].idx) return false;
    else if (b.coverList[0].idx > a.coverList[0].idx) return true;
    return (b.coverList[0].id > a.coverList[0].id);
}

inline unsigned short tileDist(unsigned short ax, unsigned short bx, unsigned short ay, unsigned short by) noexcept {
    return (ax > bx ? ax - bx : bx - ax) + (ay > by ? ay - by : by - ay);
}

struct CoalesceBaton : carmen::noncopyable {
    uv_work_t request;
    // params
    std::vector<PhrasematchSubq> stack;
    std::vector<unsigned short> centerzxy;
    v8::Persistent<v8::Function> callback;
    // return
    std::vector<Context> features;
};

void coalesceFinalize(CoalesceBaton* baton, std::vector<Context> const& contexts) {
    if (contexts.size() > 0) {
        // Coalesce stack, generate relevs.
        double relevMax = contexts[0].relev;
        unsigned short total = 0;
        std::map<uint64_t,bool> sets;
        std::map<uint64_t,bool>::iterator sit;

        for (unsigned short i = 0; i < contexts.size(); i++) {
            // Maximum allowance of coalesced features: 40.
            if (total >= 40) break;

            Context const& feature = contexts[i];

            // Since `coalesced` is sorted by relev desc at first
            // threshold miss we can break the loop.
            if (relevMax - feature.relev >= 0.25) break;

            // Only collect each feature once.
            sit = sets.find(feature.coverList[0].tmpid);
            if (sit != sets.end()) continue;

            sets.emplace(feature.coverList[0].tmpid, true);
            baton->features.emplace_back(feature);
            total++;
        }
    }
}
void coalesceSingle(uv_work_t* req) {
    CoalesceBaton *baton = static_cast<CoalesceBaton *>(req->data);

    std::vector<PhrasematchSubq> const& stack = baton->stack;
    PhrasematchSubq const& subq = stack[0];
    std::string type = "grid";
    std::string shardId = shard(subq.shardlevel, subq.phrase);

    // proximity (optional)
    bool proximity = !baton->centerzxy.empty();
    unsigned short cx;
    unsigned short cy;
    if (proximity) {
        cx = baton->centerzxy[1];
        cy = baton->centerzxy[2];
    } else {
        cx = 0;
        cy = 0;
    }

    // sort grids by distance to proximity point
    Cache::intarray const& grids = __get(subq.cache, type, shardId, subq.phrase);

    unsigned long m = grids.size();
    double relevMax = 0;
    std::vector<Cover> covers;
    covers.reserve(m);

    for (unsigned long j = 0; j < m; j++) {
        Cover cover = numToCover(grids[j]);
        cover.idx = subq.idx;
        cover.tmpid = static_cast<uint32_t>(cover.idx * POW2_25 + cover.id);
        cover.relev = cover.relev * subq.weight;
        cover.distance = proximity ? tileDist(cx, cover.x, cy, cover.y) : 0;

        // short circuit based on relevMax thres
        if (relevMax - cover.relev >= 0.25) continue;
        if (cover.relev > relevMax) relevMax = cover.relev;

        covers.emplace_back(cover);
    }

    if (proximity) {
        std::sort(covers.begin(), covers.end(), coverSortByRelevDistance);
    } else {
        std::sort(covers.begin(), covers.end(), coverSortByRelev);
    }

    std::vector<Context> contexts;
    m = covers.size() > 40 ? 40 : covers.size();
    contexts.reserve(m);
    for (unsigned long j = 0; j < m; j++) {
        Context context;
        context.coverList.emplace_back(covers[j]);
        context.relev = covers[j].relev;
        contexts.emplace_back(context);
    }

    coalesceFinalize(baton, contexts);
}

void coalesceMulti(uv_work_t* req) {
    CoalesceBaton *baton = static_cast<CoalesceBaton *>(req->data);
    std::vector<PhrasematchSubq> const& stack = baton->stack;

    size_t size;

    // Cache zoom levels to iterate over as coalesce occurs.
    Cache::intarray zoom;
    std::vector<bool> zoomUniq(22);
    std::vector<Cache::intarray> zoomCache(22);
    size = stack.size();
    for (unsigned short i = 0; i < size; i++) {
        if (zoomUniq[stack[i].zoom]) continue;
        zoomUniq[stack[i].zoom] = true;
        zoom.emplace_back(stack[i].zoom);
    }

    size = zoom.size();
    for (unsigned short i = 0; i < size; i++) {
        Cache::intarray sliced;
        sliced.reserve(i);
        for (unsigned short j = 0; j < i; j++) {
            sliced.emplace_back(zoom[j]);
        }
        std::reverse(sliced.begin(), sliced.end());
        zoomCache[zoom[i]] = sliced;
    }

    // Coalesce relevs into higher zooms, e.g.
    // z5 inherits relev of overlapping tiles at z4.
    // @TODO assumes sources are in zoom ascending order.
    std::string type = "grid";
    std::map<uint64_t,std::vector<Cover>> coalesced;
    std::map<uint64_t,std::vector<Cover>>::iterator cit;
    std::map<uint64_t,std::vector<Cover>>::iterator pit;
    std::map<uint64_t,bool> done;
    std::map<uint64_t,bool>::iterator dit;

    size = stack.size();

    // proximity (optional)
    bool proximity = baton->centerzxy.size() > 0;
    unsigned short cz;
    unsigned short cx;
    unsigned short cy;
    if (proximity) {
        cz = baton->centerzxy[0];
        cx = baton->centerzxy[1];
        cy = baton->centerzxy[2];
    } else {
        cz = 0;
        cx = 0;
        cy = 0;
    }

    for (unsigned short i = 0; i < size; i++) {
        PhrasematchSubq const& subq = stack[i];

        std::string shardId = shard(subq.shardlevel, subq.phrase);

        Cache::intarray const& grids = __get(subq.cache, type, shardId, subq.phrase);

        unsigned short z = subq.zoom;
        auto const& zCache = zoomCache[z];
        std::size_t zCacheSize = zCache.size();

        unsigned long m = grids.size();

        for (unsigned long j = 0; j < m; j++) {
            Cover cover = numToCover(grids[j]);
            cover.idx = subq.idx;
            cover.tmpid = static_cast<uint32_t>(cover.idx * POW2_25 + cover.id);
            cover.relev = cover.relev * subq.weight;
            if (proximity) {
                ZXY dxy = pxy2zxy(z, cover.x, cover.y, cz);
                cover.distance = tileDist(cx, dxy.x, cy, dxy.y);
            } else {
                cover.distance = 0;
            }

            uint64_t zxy = (z * POW2_28) + (cover.x * POW2_14) + (cover.y);

            cit = coalesced.find(zxy);
            if (cit == coalesced.end()) {
                std::vector<Cover> coverList;
                coverList.push_back(cover);
                coalesced.emplace(zxy, coverList);
            } else {
                cit->second.push_back(cover);
            }

            dit = done.find(zxy);
            if (dit == done.end()) {
                for (unsigned a = 0; a < zCacheSize; a++) {
                    uint64_t p = zCache[a];
                    double s = static_cast<double>(1 << (z-p));
                    uint64_t pxy = static_cast<uint64_t>((p * POW2_28) + (std::floor(cover.x/s) * POW2_14) + std::floor(cover.y/s));
                    // Set a flag to ensure coalesce occurs only once per zxy.
                    pit = coalesced.find(pxy);
                    if (pit != coalesced.end()) {
                        cit = coalesced.find(zxy);
                        for (auto const& pArray : pit->second) {
                            cit->second.emplace_back(pArray);
                        }
                        done.emplace(zxy, true);
                        break;
                    }
                }
            }
        }
    }

    std::vector<Context> contexts;
    for (auto const& matched : coalesced) {
        std::vector<Cover> const& coverList = matched.second;
        size_t coverSize = coverList.size();
        for (unsigned short i = 0; i < coverSize; i++) {
            unsigned short used = 1 << coverList[i].idx;
            double stacky = 0.0;

            Context context;
            context.coverList.emplace_back(coverList[i]);
            context.relev = coverList[i].relev;
            for (unsigned short j = i+1; j < coverSize; j++) {
                unsigned short mask = 1 << coverList[j].idx;
                if (used & mask) continue;
                stacky = 1.0;
                used = used | mask;
                context.coverList.emplace_back(coverList[j]);
                context.relev += coverList[j].relev;
            }
            context.relev -= 0.01;
            context.relev += 0.01 * stacky;
            contexts.emplace_back(context);
        }
    }
    if (proximity) {
        std::sort(contexts.begin(), contexts.end(), contextSortByRelevDistance);
    } else {
        std::sort(contexts.begin(), contexts.end(), contextSortByRelev);
    }
    coalesceFinalize(baton, contexts);
}

Local<Object> coverToObject(Cover const& cover) {
    Local<Object> object = NanNew<Object>();
    object->Set(NanNew("x"), NanNew<Number>(cover.x));
    object->Set(NanNew("y"), NanNew<Number>(cover.y));
    object->Set(NanNew("relev"), NanNew<Number>(cover.relev));
    object->Set(NanNew("score"), NanNew<Number>(cover.score));
    object->Set(NanNew("id"), NanNew<Number>(cover.id));
    object->Set(NanNew("idx"), NanNew<Number>(cover.idx));
    object->Set(NanNew("tmpid"), NanNew<Number>(cover.tmpid));
    object->Set(NanNew("distance"), NanNew<Number>(cover.distance));
    return object;
}
Local<Array> contextToArray(Context const& context) {
    std::size_t size = context.coverList.size();
    Local<Array> array = NanNew<Array>(static_cast<int>(size));
    for (uint32_t i = 0; i < size; i++) {
        array->Set(i, coverToObject(context.coverList[i]));
    }
    array->Set(NanNew("relev"), NanNew(context.relev));
    return array;
}
void coalesceAfter(uv_work_t* req) {
    NanScope();
    CoalesceBaton *baton = static_cast<CoalesceBaton *>(req->data);
    std::vector<Context> const& features = baton->features;

    Local<Array> jsFeatures = NanNew<Array>(static_cast<int>(features.size()));
    for (unsigned short i = 0; i < features.size(); i++) {
        jsFeatures->Set(i, contextToArray(features[i]));
    }

    Local<Value> argv[2] = { NanNull(), jsFeatures };
    NanMakeCallback(NanGetCurrentContext()->Global(), NanNew(baton->callback), 2, argv);
    NanDisposePersistent(baton->callback);
    delete baton;
}
NAN_METHOD(Cache::coalesce) {
    NanScope();

    // PhrasematchStack (js => cpp)
    if (!args[0]->IsArray()) {
        return NanThrowTypeError("Arg 1 must be a PhrasematchSubq array");
    }
    CoalesceBaton *baton = new CoalesceBaton();

    try {
        std::vector<PhrasematchSubq> stack;
        const Local<Array> array = Local<Array>::Cast(args[0]);
        for (uint32_t i = 0; i < array->Length(); i++) {
            Local<Object> jsStack = Local<Object>::Cast(array->Get(i));
            PhrasematchSubq subq;

            int64_t _idx = jsStack->Get(NanNew("idx"))->IntegerValue();
            if (_idx < 0 || _idx > std::numeric_limits<unsigned short>::max()) {
                delete baton;
                return NanThrowTypeError("encountered idx value too large to fit in unsigned short");
            }
            subq.idx = static_cast<unsigned short>(_idx);

            int64_t _zoom = jsStack->Get(NanNew("zoom"))->IntegerValue();
            if (_zoom < 0 || _zoom > std::numeric_limits<unsigned short>::max()) {
                delete baton;
                return NanThrowTypeError("encountered zoom value too large to fit in unsigned short");
            }
            subq.zoom = static_cast<unsigned short>(_zoom);

            subq.weight = jsStack->Get(NanNew("weight"))->NumberValue();
            int64_t _phrase = jsStack->Get(NanNew("phrase"))->IntegerValue();
            if (_phrase < 0 || _phrase > std::numeric_limits<uint32_t>::max()) {
                delete baton;
                return NanThrowTypeError("encountered phrase value too large to fit in unsigned short");
            }
            subq.phrase = static_cast<uint32_t>(_phrase);
            subq.shardlevel = static_cast<uint64_t>(jsStack->Get(NanNew("shardlevel"))->NumberValue());

            // JS cache reference => cpp
            Local<Object> cache = Local<Object>::Cast(jsStack->Get(NanNew("cache")));
            subq.cache = node::ObjectWrap::Unwrap<Cache>(cache);

            stack.push_back(subq);
        }
        baton->stack = stack;

        // Options object (js => cpp)
        if (!args[1]->IsObject()) {
            delete baton;
            return NanThrowTypeError("Arg 2 must be an options object");
        }
        const Local<Object> options = Local<Object>::Cast(args[1]);
        if (options->Has(NanNew("centerzxy"))) {
            baton->centerzxy = arrayToVector(Local<Array>::Cast(options->Get(NanNew("centerzxy"))));
        }

        // callback
        if (!args[2]->IsFunction()) {
            delete baton;
            return NanThrowTypeError("Arg 3 must be a callback function");
        }
        Local<Value> callback = args[2];
        NanAssignPersistent(baton->callback, callback.As<Function>());

        // queue work
        baton->request.data = baton;
        // optimization: for stacks of 1, use coalesceSingle
        if (stack.size() == 1) {
            uv_queue_work(uv_default_loop(), &baton->request, coalesceSingle, (uv_after_work_cb)coalesceAfter);
        } else {
            uv_queue_work(uv_default_loop(), &baton->request, coalesceMulti, (uv_after_work_cb)coalesceAfter);
        }
    } catch (std::exception const& ex) {
        delete baton;
        return NanThrowTypeError(ex.what());
    }

    NanReturnUndefined();
}

extern "C" {
    static void start(Handle<Object> target) {
        Cache::Initialize(target);
    }
}

} // namespace carmen


NODE_MODULE(carmen, carmen::start)
