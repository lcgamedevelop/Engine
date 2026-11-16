#if defined(ENGINE_HAS_TERRAIN_PLUGIN) && ENGINE_HAS_TERRAIN_PLUGIN
#include "plugins/runtime/script/bindings/terrain_binding.hpp"
#include "plugins/runtime/script/bindings/control_binding_helpers.hpp"
#include "plugins/runtime/terrain/terrain_control.hpp"

namespace engine::plugins::script::bindings
{
namespace
{
constexpr std::string_view javascript = R"js(
function terrainInput(value) {
    if (typeof value === 'bigint') return value.toString();
    if (Array.isArray(value)) return value.map(terrainInput);
    if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([k,v])=>[k.replace(/[A-Z]/g,m=>'_'+m.toLowerCase()),terrainInput(v)]));
    return value;
}
const terrainIntegers = new Set(['world_epoch','node_id','component_instance','sequence','revision','settings_revision','fragment_count','base_revision','published_revision','content_revision','captured_revision','density_revision','mesh_revision','collision_revision','completed','total']);
function terrainOutput(value,key='') {
    if (typeof value==='string' && terrainIntegers.has(key)) return BigInt(value);
    if (Array.isArray(value)) return value.map(v=>terrainOutput(v));
    if (value && typeof value==='object') return Object.fromEntries(Object.entries(value).map(([k,v])=>[k.replace(/_([a-z])/g,(_,c)=>c.toUpperCase()),terrainOutput(v,k)]));
    return value;
}
function terrainCall(component,method,args) {
    const h=handles.get(component);
    return terrainOutput(native.TerrainCall(h.scene,h.node,method,JSON.stringify(terrainInput(args))));
}
export class TerrainComponent extends Component {
    constructor(token,scene,node){super(token,scene,node,native.TerrainKind);}
    GetSettings(){const h=handles.get(this);return copyComponentData(native.TerrainSettings(h.scene,h.node));}
    SetSettings(settings){const h=handles.get(this);return native.TerrainSetSettings(h.scene,h.node,JSON.stringify(settings,(_,v)=>typeof v==='bigint'?v.toString():v));}
    SetView(key,view){return terrainCall(this,'set_view',{viewKey:key,view})===true;}
    RemoveView(key){return terrainCall(this,'remove_view',{viewKey:key})===true;}
    PinRegions(spec){return terrainCall(this,'pin_regions',spec);}
    ReleasePin(id){return terrainCall(this,'release_pin',id)===true;}
    RequestEdit(spec){return terrainCall(this,'request_edit',spec);}
    RequestCollapse(spec){return terrainCall(this,'request_collapse',spec);}
    RequestReset(spec){return terrainCall(this,'request_reset',spec);}
    RequestRetire(spec){return terrainCall(this,'request_retire',spec);}
    RequestSave(spec){return terrainCall(this,'request_save',spec);}
    RequestLoad(spec){return terrainCall(this,'request_load',spec);}
    GetOperation(id){return terrainCall(this,'operation_status',id);}
    CancelOperation(id){return terrainCall(this,'cancel_operation',id)===true;}
    ReleaseOperation(id){return terrainCall(this,'release_operation',id)===true;}
    GetRegions(query){return terrainCall(this,'query_regions',query);}
    CollectSupport(query){
        const result=terrainCall(this,'collect_support',query);
        if(result===null)return null;
        const h=handles.get(this);
        return result.map(value=>new Node(key,h.scene,value.nodeId).GetComponent(Collider));
    }
}
componentTypes.set(TerrainComponent,{add:native.TerrainAdd,kind:native.TerrainKind});
Object.freeze(TerrainComponent.prototype);Object.freeze(TerrainComponent);
)js";
void call(const Arguments& args)
{
    auto component = binding_detail::component<terrain::TerrainComponent>(args, 4);
    auto* context = BindingContext::current(args);
    args.set_return({});
    std::string method, error;
    control_detail::Json input, output;
    if (!component || !context || !BindingContext::text(args, 2, method) ||
        !control_detail::argument(args, 3, input) ||
        !terrain::control::call(component, method, input, output, error))
    {
        return;
    }
    const auto prefix = "terrain/" + std::to_string(component.node().id()) + "/" +
                        std::to_string(component.instance_id()) + "/";
    if (method == "set_view")
    {
        const auto cleanup = input;
        const auto key = prefix + "view/" + input["view_key"].dump();
        context->remember_release(key,
                                  [component, cleanup]()
                                  {
                                      control_detail::Json result;
                                      std::string error;
                                      (void)terrain::control::call(component, "remove_view",
                                                                   cleanup, result, error);
                                  });
    }
    else if (method == "remove_view")
    {
        context->forget_release(prefix + "view/" + input["view_key"].dump());
    }
    else if (method == "pin_regions")
    {
        const auto id = output;
        context->remember_release(prefix + "pin/" + id["sequence"].dump(),
                                  [component, id]()
                                  {
                                      control_detail::Json result;
                                      std::string error;
                                      (void)terrain::control::call(component, "release_pin", id,
                                                                   result, error);
                                  });
    }
    else if (method == "release_pin")
    {
        context->forget_release(prefix + "pin/" + input["sequence"].dump());
    }
    else if (method.starts_with("request_"))
    {
        const auto id = output;
        context->remember_release(prefix + "operation/" + id["sequence"].dump(), [component, id]()
                                  { terrain::control::abandon_operation(component, id); });
    }
    else if (method == "release_operation")
    {
        context->forget_release(prefix + "operation/" + input["sequence"].dump());
    }
    args.set_return(control_detail::native(output));
}
} // namespace
std::string_view TerrainBinding::name() const noexcept
{
    return "TerrainComponent";
}
std::string_view TerrainBinding::source() const noexcept
{
    return javascript;
}
bool TerrainBinding::register_functions(vm::ScriptVm& vm) const
{
    return vm.integer("TerrainKind", static_cast<int>(terrain::TerrainComponent::component_type)) &&
           vm.function("TerrainAdd", binding_detail::add_component<terrain::TerrainComponent>) &&
           vm.function("TerrainCall", call) &&
           vm.function("TerrainSettings",
                       control_detail::get_settings<terrain::TerrainComponent>) &&
           vm.function("TerrainSetSettings",
                       control_detail::set_settings<terrain::TerrainComponent>);
}
} // namespace engine::plugins::script::bindings
#endif
