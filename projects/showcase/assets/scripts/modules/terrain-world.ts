import { Debug, Gameplay, Resources, ResourceHandle, Scene, TerrainComponent, Transform, UiComponent } from "engine";
import { TerrainWorldModel, world_stops } from "../models/terrain_world_model.js";
import { TerrainWorldController } from "../controllers/terrain_world_controller.js";

export default class TerrainWorld extends Gameplay {
    scene: Scene | null = null;
    private readonly resources: ResourceHandle[] = [];
    private terrain: TerrainComponent | null = null;
    private camera: Transform | null = null;
    private controller: TerrainWorldController | null = null;
    private readonly model = new TerrainWorldModel();
    private currentView = 1n;
    private nextView = 2n;
    private destination: number | null = null;

    override OnInitialize(): boolean {
        for (const path of ["scenes/terrain-world.scene", "ui/terrain-world.rml"]) {
            const resource = Resources.Load(path);
            if (!resource) return false;
            this.resources.push(resource);
        }
        this.scene = Scene.Load("scenes/terrain-world.scene");
        this.terrain = this.scene?.FindNode("World terrain")?.GetComponent(TerrainComponent) ?? null;
        this.camera = this.scene?.FindNode("World camera")?.GetComponent(Transform) ?? null;
        const view = this.scene?.CreateNode("World controls")?.AddComponent(UiComponent);
        if (!view || !this.terrain || !this.camera || !view.SetDocument("ui/terrain-world.rml") ||
            !this.terrain.SetView(this.currentView, {center:{x:-3600,y:0,z:-3600},visualRadius:1200,collisionRadius:128,lodBias:0,priority:0})) return false;
        this.controller = new TerrainWorldController(view, this.model);
        return this.controller.Bind();
    }

    override Update(): void {
        const terrain = this.terrain;
        if (!terrain) return;
        if (this.model.pending !== null && this.destination === null) {
            const index = this.model.pending;
            this.model.pending = null;
            const stop = world_stops[index];
            if (!stop || !terrain.SetView(this.nextView,
                {center:{x:stop.x,y:0,z:stop.z},visualRadius:1200,collisionRadius:128,lodBias:0,priority:0})) {
                this.model.status = "Cannot request the target regions";
            } else {
                this.destination = index;
                this.model.busy = true;
                this.model.status = `Preparing ${stop.name}; keeping the current region visible...`;
            }
            this.controller?.Refresh();
        }
        if (this.destination === null) return;
        const stop = world_stops[this.destination];
        const state = terrain.GetRegions({bounds:{minimum:{x:stop.x-128,y:-128,z:stop.z-128},
            maximum:{x:stop.x+128,y:128,z:stop.z+128}},limit:256});
        const failed = state?.state === "failed" || state?.state === "cancelled";
        if (failed) {
            terrain.RemoveView(this.nextView);
            this.model.status = `Region preparation failed: ${state?.errorCode}: ${state?.error}`;
        } else if (state?.state === "ready" && state.regions.length && !state.nextCursor && state.regions.every(
            region => region.known && region.state === "ready" && region.meshRevision === region.densityRevision &&
                (!region.collisionRequired || region.collisionRevision === region.densityRevision))) {
            if (!this.camera?.SetPosition(stop.x+340,250,stop.z+390) || !terrain.RemoveView(this.currentView)) {
                this.model.status = "Cannot switch the prepared region";
            } else {
                this.currentView = this.nextView++;
                this.model.status = `${stop.name} (${stop.x}, ${stop.z}) / revision ${state.revision}`;
            }
        } else return;
        this.destination = null;
        this.model.busy = false;
        this.controller?.Refresh();
    }

    override OnShutdown(): void {
        this.controller?.Unbind();
        this.controller = null;
        this.terrain = null;
        this.camera = null;
        this.scene = null;
        for (const resource of this.resources.splice(0))
            if (!Resources.Release(resource)) Debug.LogError("Cannot release terrain world resource");
    }
}
