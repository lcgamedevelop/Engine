import { Resources, ResourceHandle, Debug, EnvironmentComponent, Gameplay, MeshRenderer, Scene, Transform, UiComponent } from "engine";
import { MeshletController } from "../controllers/meshlet_controller.js";
import { MeshletModel } from "../models/meshlet_model.js";

export default class MeshletCulling extends Gameplay {
    private readonly resources: ResourceHandle[] = [];
    scene: Scene | null = null;
    private readonly model = new MeshletModel();
    private controller: MeshletController | null = null;
    private environment: EnvironmentComponent | null = null;
    private readonly meshes: MeshRenderer[] = [];

    override OnInitialize(): boolean {
        for (const path of ["scenes/meshlet-culling.scene", "ui/meshlet-culling.rml",
            "meshes/meshlet-sphere.mesh", "meshes/meshlet-arch.mesh", "meshes/meshlet-wall.mesh",
            "meshes/meshlet-lod-sphere.mesh", "meshes/cube.mesh",
            "builtin://materials/default-pbr", "materials/meshlet-direct.material"]) {
            const resource = Resources.Load(path);
            if (!resource) { Debug.LogError(`Cannot load ${path}: ${Resources.lastError}`); return false; }
            this.resources.push(resource);
        }
        this.scene = Scene.Load("scenes/meshlet-culling.scene");
        this.environment = this.scene?.FindNode("Environment")?.GetComponent(EnvironmentComponent) ?? null;
        const view = this.scene?.CreateNode("Meshlet controls")?.AddComponent(UiComponent);
        if (!this.environment || !view || !view.SetDocument("ui/meshlet-culling.rml")) return false;
        for (let row = 0; row < 3; ++row) for (let column = 0; column < 6; ++column) {
            if (!this.Add(`Cluster sphere ${row}:${column}`, "meshes/meshlet-lod-sphere.mesh",
                [(column - 2.5) * 3.4, 1.3, -row * 4.2], [1.4, 1.4, 1.4], [0.28, 0.52, 0.70])) return false;
        }
        if (!this.Add("Nonuniform sphere", "meshes/meshlet-sphere.mesh", [-5, 1.2, 4],
            [1.5, 0.7, 1.0], [0.84, 0.39, 0.16]) ||
            !this.Add("Mirrored sphere", "meshes/meshlet-sphere.mesh", [0, 1.3, 4],
            [-1, 1, 1], [0.84, 0.39, 0.16]) ||
            !this.Add("Single instance sphere", "meshes/meshlet-sphere.mesh", [5, 1.3, 4],
            [1, 1, 1], [0.84, 0.39, 0.16], false)) return false;
        for (const x of [-9, -3, 3, 9]) {
            if (!this.Add(`Arch ${x}`, "meshes/meshlet-arch.mesh", [x, 0.3, -14],
                [1, 1, 1], [0.72, 0.69, 0.61])) return false;
        }
        const wall = this.Add("Long double-sided surface", "meshes/meshlet-wall.mesh",
            [0, 3, -18], [1, 1, 1], [0.35, 0.60, 0.43]);
        if (!wall || !wall.material.SetCullMode("none") ||
            !wall.node.GetComponent(Transform)?.SetRotation(90, 0, 0)) return false;
        if (!this.Add("Ordinary geometry ground", "meshes/cube.mesh", [0, -0.25, -7],
            [22, 0.25, 19], [0.32, 0.35, 0.40])) return false;
        const direct = this.Add("Shader without meshlet support", "meshes/meshlet-sphere.mesh",
            [-8, 1.3, 4], [1, 1, 1], [0.7, 0.35, 0.5]);
        if (!direct || !direct.material.SetType("materials/meshlet-direct.material")) return false;
        const transparent = this.Add("Transparent control", "meshes/meshlet-sphere.mesh",
            [8, 1.3, 4], [1, 1, 1], [0.65, 0.75, 0.9]);
        if (!transparent || !transparent.material.SetBlendMode("alpha_blend") ||
            !transparent.material.SetV4("baseColor", 0.65, 0.75, 0.9, 0.4)) return false;
        this.controller = new MeshletController(view, this.model);
        this.model.enabled = this.environment.meshletCullingEnabled;
        return this.controller.Bind();
    }

    private Add(name: string, path: string, position: readonly [number, number, number],
        scale: readonly [number, number, number], color: readonly [number, number, number],
        instancing = true): MeshRenderer | null {
        const node = this.scene?.CreateNode(name);
        const transform = node?.GetComponent(Transform);
        const mesh = node?.AddComponent(MeshRenderer);
        if (!transform || !mesh || !transform.SetPosition(...position) || !transform.SetScale(...scale) ||
            !mesh.SetMesh(path) || !mesh.material.SetType("builtin://materials/default-pbr") ||
            !mesh.material.SetV4("baseColor", ...color, 1) ||
            !mesh.material.SetFloat("roughness", 0.55) || !mesh.SetStaticInstancing(instancing) ||
            !mesh.SetStaticLod(true)) return null;
        if (path === "meshes/meshlet-lod-sphere.mesh") {
            if (!mesh.SetMaterialCount(2)) return null;
            const second = mesh.GetMaterial(1);
            if (!second || !second.SetType("builtin://materials/default-pbr") ||
                !second.SetV4("baseColor", 0.18, 0.34, 0.52, 1) ||
                !second.SetFloat("roughness", 0.55)) return null;
        }
        this.meshes.push(mesh);
        return mesh;
    }

    Inspect() {
        return { enabled: this.environment?.meshletCullingEnabled ?? false, objects: this.meshes.length };
    }

    override Update(): void {
        if (!this.environment || !this.controller) return;
        if (this.model.toggle) {
            this.model.toggle = false;
            if (!this.environment.SetMeshletCulling(!this.environment.meshletCullingEnabled))
                this.model.status = "Cannot change the scene environment.";
        }
        this.model.enabled = this.environment.meshletCullingEnabled;
        if (!this.controller.Refresh()) Debug.LogError("Meshlet controls refresh failed");
    }

    override OnShutdown(): void {
        this.controller?.Unbind();
        this.controller = null;
        this.environment = null;
        this.meshes.length = 0;
        this.scene = null;
        for (const resource of this.resources.splice(0)) {
            if (!Resources.Release(resource)) Debug.LogError("Cannot release meshlet showcase resource");
        }
    }
}
