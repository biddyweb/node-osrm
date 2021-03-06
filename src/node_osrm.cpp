// v8
#include <nan.h>

// OSRM
#include <osrm/OSRM.h>
#include <osrm/RouteParameters.h>
#include <osrm/Reply.h>
#include <osrm/ServerPaths.h>

// STL
#include <iostream>
#include <memory>

namespace node_osrm {

typedef std::unique_ptr<OSRM> osrm_ptr;
typedef std::unique_ptr<RouteParameters> route_parameters_ptr;

namespace {
template <class T, class... Types>
std::unique_ptr<T> make_unique(Types &&... Args)
{
    return (std::unique_ptr<T>(new T(std::forward<Types>(Args)...)));
}
}

using namespace v8;

class Engine final : public node::ObjectWrap {
public:
    static Persistent<FunctionTemplate> constructor;
    static void Initialize(Handle<Object>);
    static NAN_METHOD(New);
    static NAN_METHOD(route);
    static NAN_METHOD(locate);
    static NAN_METHOD(nearest);
    static NAN_METHOD(table);

    static void Run(_NAN_METHOD_ARGS, route_parameters_ptr);
    static void AsyncRun(uv_work_t*);
    static void AfterRun(uv_work_t*);

private:
    Engine(const ServerPaths &paths, bool use_shared_memory)
        : ObjectWrap(),
          this_(make_unique<OSRM>(paths, use_shared_memory))
    {}

    osrm_ptr this_;
};

Persistent<FunctionTemplate> Engine::constructor;

void Engine::Initialize(Handle<Object> target) {
    NanScope();
    Local<FunctionTemplate> lcons = NanNew<FunctionTemplate>(Engine::New);
    lcons->InstanceTemplate()->SetInternalFieldCount(1);
    lcons->SetClassName(NanNew("OSRM"));
    NODE_SET_PROTOTYPE_METHOD(lcons, "route", route);
    NODE_SET_PROTOTYPE_METHOD(lcons, "locate", locate);
    NODE_SET_PROTOTYPE_METHOD(lcons, "nearest", nearest);
    NODE_SET_PROTOTYPE_METHOD(lcons, "table", table);
    target->Set(NanNew("OSRM"), lcons->GetFunction());
    NanAssignPersistent(constructor, lcons);
}

NAN_METHOD(Engine::New)
{
    NanScope();

    if (!args.IsConstructCall()) {
        NanThrowTypeError("Cannot call constructor as function, you need to use 'new' keyword");
        NanReturnUndefined();
    }

    try {
        ServerPaths paths;
        if (args.Length() == 1) {
            if (!args[0]->IsString()) {
                NanThrowError("OSRM base path must be a string");
                NanReturnUndefined();
            }
            std::string base = *String::Utf8Value(args[0]->ToString());
            paths["base"] = base;
        }

        auto  im = new Engine(paths, args.Length() == 0);
        im->Wrap(args.This());
        NanReturnValue(args.This());
    } catch (std::exception const& ex) {
         NanThrowTypeError(ex.what());
         NanReturnUndefined();
    }
}

struct RunQueryBaton {
    uv_work_t request;
    Engine * machine;
    std::string result;
    route_parameters_ptr params;
    Persistent<Function> cb;
    bool error;
};

NAN_METHOD(Engine::route)
{
    NanScope();

    if (args.Length() < 2) {
        NanThrowTypeError("two arguments required");
        NanReturnUndefined();
    }

    if (!args[0]->IsObject()) {
        NanThrowTypeError("two arguments required");
        NanReturnUndefined();
    }

    Local<Object> obj = args[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined()) {
        NanThrowError("first arg must be an object");
        NanReturnUndefined();
    }

    route_parameters_ptr params = make_unique<RouteParameters>();

    params->zoom_level = 18; //no generalization
    params->print_instructions = false; //turn by turn instructions
    params->alternate_route = true; //get an alternate route, too
    params->geometry = true; //retrieve geometry of route
    params->compression = true; //polyline encoding
    params->check_sum = 0; //see wiki
    params->service = "viaroute"; //that's routing
    params->output_format = "json";
    params->jsonp_parameter = ""; //set for jsonp wrapping
    params->language = ""; //unused atm

    if (!obj->Has(NanNew("coordinates"))) {
        NanThrowError("must provide a coordinates property");
        NanReturnUndefined();
    }

    Local<Value> coordinates = obj->Get(NanNew("coordinates"));
    if (!coordinates->IsArray()) {
        NanThrowError("coordinates must be an array of (lat/long) pairs");
        NanReturnUndefined();
    }

    Local<Array> coordinates_array = Local<Array>::Cast(coordinates);
    if (coordinates_array->Length() < 2) {
        NanThrowError("at least two coordinates must be provided");
        NanReturnUndefined();
    }

    for (uint32_t i = 0; i < coordinates_array->Length(); ++i) {
        Local<Value> coordinate = coordinates_array->Get(i);

        if (!coordinate->IsArray()) {
            NanThrowError("coordinates must be an array of (lat/long) pairs");
            NanReturnUndefined();
        }

        Local<Array> coordinate_pair = Local<Array>::Cast(coordinate);
        if (coordinate_pair->Length() != 2) {
            NanThrowError("coordinates must be an array of (lat/long) pairs");
            NanReturnUndefined();
        }

        params->coordinates.emplace_back(static_cast<int>(coordinate_pair->Get(0)->NumberValue()*COORDINATE_PRECISION),
                                         static_cast<int>(coordinate_pair->Get(1)->NumberValue()*COORDINATE_PRECISION));
    }

    if (obj->Has(NanNew("alternateRoute"))) {
        params->alternate_route = obj->Get(NanNew("alternateRoute"))->BooleanValue();
    }

    if (obj->Has(NanNew("checksum"))) {
        params->check_sum = static_cast<unsigned>(obj->Get(NanNew("checksum"))->Uint32Value());
    }

    if (obj->Has(NanNew("zoomLevel"))) {
        params->zoom_level = static_cast<short>(obj->Get(NanNew("zoomLevel"))->Int32Value());
    }

    if (obj->Has(NanNew("printInstructions"))) {
        params->print_instructions = obj->Get(NanNew("printInstructions"))->BooleanValue();
    }

    if (obj->Has(NanNew("jsonpParameter"))) {
        params->jsonp_parameter = *v8::String::Utf8Value(obj->Get(NanNew("jsonpParameter")));
    }

    if (obj->Has(NanNew("hints"))) {
        Local<Value> hints = obj->Get(NanNew("hints"));

        if (!hints->IsArray()) {
            NanThrowError("hints must be an array of strings/null");
            NanReturnUndefined();
        }

        Local<Array> hints_array = Local<Array>::Cast(hints);
        for (uint32_t i = 0; i < hints_array->Length(); ++i) {
            Local<Value> hint = hints_array->Get(i);
            if (hint->IsString()) {
                params->hints.push_back(*v8::String::Utf8Value(hint));
            } else if(hint->IsNull()){
                params->hints.push_back("");
            }else{
                NanThrowError("hint must be null or string");
                NanReturnUndefined();
            }
        }
    }

    Run(args, std::move(params));
    NanReturnUndefined();
}

NAN_METHOD(Engine::locate)
{
    NanScope();
    if (args.Length() < 2) {
        NanThrowTypeError("two arguments required");
        NanReturnUndefined();
    }

    Local<Value> coordinate = args[0];
    if (!coordinate->IsArray()) {
        NanThrowError("first argument must be an array of lat, long");
        NanReturnUndefined();
    }

    Local<Array> coordinate_pair = Local<Array>::Cast(coordinate);
    if (coordinate_pair->Length() != 2) {
        NanThrowError("first argument must be an array of lat, long");
        NanReturnUndefined();
    }

    route_parameters_ptr params = make_unique<RouteParameters>();

    params->service = "locate";
    params->coordinates.emplace_back(static_cast<int>(coordinate_pair->Get(0)->NumberValue()*COORDINATE_PRECISION),
                                     static_cast<int>(coordinate_pair->Get(1)->NumberValue()*COORDINATE_PRECISION));

    Run(args, std::move(params));
    NanReturnUndefined();
}

NAN_METHOD(Engine::table)
{
    NanScope();
    if (args.Length() < 2) {
        NanThrowTypeError("two arguments required");
        NanReturnUndefined();
    }

    Local<Object> obj = args[0]->ToObject();
    if (obj->IsNull() || obj->IsUndefined()) {
        NanThrowError("first arg must be an object");
        NanReturnUndefined();
    }

    Local<Value> coordinates = obj->Get(NanNew("coordinates"));
    if (!coordinates->IsArray()) {
        NanThrowError("coordinates must be an array of (lat/long) pairs");
        NanReturnUndefined();
    }

    Local<Array> coordinates_array = Local<Array>::Cast(coordinates);
    if (coordinates_array->Length() < 2) {
        NanThrowError("at least two coordinates must be provided");
        NanReturnUndefined();
    }

    route_parameters_ptr params = make_unique<RouteParameters>();
    params->service = "table";

    // add all coordinates
    for (uint32_t i = 0; i < coordinates_array->Length(); ++i) {
        Local<Value> coordinate = coordinates_array->Get(i);

        if (!coordinate->IsArray()) {
            NanThrowError("coordinates must be an array of (lat/long) pairs");
            NanReturnUndefined();
        }

        Local<Array> coordinate_pair = Local<Array>::Cast(coordinate);
        if (coordinate_pair->Length() != 2) {
            NanThrowError("coordinates must be an array of (lat/long) pairs");
            NanReturnUndefined();
        }

        params->coordinates.emplace_back(static_cast<int>(coordinate_pair->Get(0)->NumberValue()*COORDINATE_PRECISION),
                                         static_cast<int>(coordinate_pair->Get(1)->NumberValue()*COORDINATE_PRECISION));
    }

    Run(args, std::move(params));
    NanReturnUndefined();
}

NAN_METHOD(Engine::nearest)
{
    NanScope();
    if (args.Length() < 2) {
        NanThrowTypeError("two arguments required");
        NanReturnUndefined();
    }

    Local<Value> coordinate = args[0];
    if (!coordinate->IsArray()) {
        NanThrowError("first argument must be an array of lat, long");
        NanReturnUndefined();
    }

    Local<Array> coordinate_pair = Local<Array>::Cast(coordinate);
    if (coordinate_pair->Length() != 2) {
        NanThrowError("first argument must be an array of lat, long");
        NanReturnUndefined();
    }

    route_parameters_ptr params = make_unique<RouteParameters>();

    params->service = "nearest";
    params->coordinates.emplace_back(static_cast<int>(coordinate_pair->Get(0)->NumberValue()*COORDINATE_PRECISION),
                                     static_cast<int>(coordinate_pair->Get(1)->NumberValue()*COORDINATE_PRECISION));
    Run(args, std::move(params));
    NanReturnUndefined();
}

void Engine::Run(_NAN_METHOD_ARGS, route_parameters_ptr params)
{
    NanScope();
    Local<Value> callback = args[args.Length()-1];

    if (!callback->IsFunction()) {
        NanThrowTypeError("last argument must be a callback function");
        return;
    }

    auto closure = new RunQueryBaton();
    closure->request.data = closure;
    closure->machine = ObjectWrap::Unwrap<Engine>(args.This());
    closure->params = std::move(params);
    closure->error = false;
    NanAssignPersistent(closure->cb, callback.As<Function>());
    uv_queue_work(uv_default_loop(), &closure->request, AsyncRun, reinterpret_cast<uv_after_work_cb>(AfterRun));
    closure->machine->Ref();
    return;
}

void Engine::AsyncRun(uv_work_t* req) {
    RunQueryBaton *closure = static_cast<RunQueryBaton *>(req->data);
    try {
        http::Reply osrm_reply;
        closure->machine->this_->RunQuery(*closure->params, osrm_reply);
        closure->result = std::string(std::begin(osrm_reply.content), std::end(osrm_reply.content));
    } catch(std::exception const& ex) {
        closure->error = true;
        closure->result = ex.what();
    }
}

void Engine::AfterRun(uv_work_t* req) {
    NanScope();
    RunQueryBaton *closure = static_cast<RunQueryBaton *>(req->data);
    TryCatch try_catch;
    if (closure->error) {
        Local<Value> argv[1] = { NanError(closure->result.c_str()) };
        NanMakeCallback(NanGetCurrentContext()->Global(), NanNew(closure->cb), 1, argv);
    } else {
        Local<Value> argv[2] = { NanNull(),
                                 NanNew(closure->result.c_str()) };
        NanMakeCallback(NanGetCurrentContext()->Global(), NanNew(closure->cb), 2, argv);
    }
    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }
    closure->machine->Unref();
    NanDisposePersistent(closure->cb);
    delete closure;
}

extern "C" {
    static void start(Handle<Object> target) {
        Engine::Initialize(target);
    }
}

} // namespace node_osrm

NODE_MODULE(osrm, node_osrm::start)
