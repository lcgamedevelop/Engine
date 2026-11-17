import { Debug, Gameplay, Light, MeshRenderer, ResourceHandle, Resources, Scene, Transform } from "engine";

export default class VirtualShadows extends Gameplay {
    private readonly resources: ResourceHandle[] = [];
    scene: Scene | null = null;

    override OnInitialize(): boolean {
        for (const path of ["scenes/virtual-shadows.scene", "meshes/cube.mesh", "builtin://materials/default-pbr"]) {
            const resource = Resources.Load(path);
            if (!resource) { Debug.LogError(`Cannot load ${path}: ${Resources.lastError}`); return false; }
            this.resources.push(resource);
        }
        this.scene = Scene.Load("scenes/virtual-shadows.scene");
        if (!this.scene) return false;
        if (!this.Add("Ground", [0, -0.3, 0], [22, 0.3, 22], [0.53, 0.56, 0.60]) ||
            !this.Add("Temple plinth", [0, 0.35, -6], [8, 0.35, 7], [0.66, 0.63, 0.57])) return false;
        for (const x of [-6, -2, 2, 6]) for (const z of [-10, -2]) {
            if (!this.Add(`Column ${x}:${z}`, [x, 3.7, z], [0.55, 3, 0.55], [0.76, 0.72, 0.64]) ||
                !this.Add(`Capital ${x}:${z}`, [x, 6.8, z], [0.8, 0.25, 0.8], [0.85, 0.80, 0.71])) return false;
        }
        for (const z of [-10, -6, -2]) {
            if (!this.Add(`Roof beam ${z}`, [0, 7.2, z], [7.2, 0.2, 0.35], [0.76, 0.72, 0.64])) return false;
        }
        for (let i = 0; i < 8; ++i) {
            if (!this.Add(`Thin caster ${i}`, [-11 + i * 0.7, 1.5, 4], [0.06, 1.5, 0.06], [0.32, 0.42, 0.50]) ||
                !this.Add(`Step ${i}`, [8, 0.15 * (i + 1), 4 - i * 0.5], [2, 0.15 * (i + 1), 0.25], [0.57, 0.61, 0.65])) return false;
        }
        return this.Add("Moving caster", [0, 1.5, 5], [1, 1.5, 1], [0.68, 0.32, 0.14]);
    }

    private Add(name: string, position: readonly [number, number, number],
        scale: readonly [number, number, number], color: readonly [number, number, number]): boolean {
        const node = this.scene?.CreateNode(name);
        const transform = node?.GetComponent(Transform);
        const mesh = node?.AddComponent(MeshRenderer);
        return !!transform && !!mesh && transform.SetPosition(...position) && transform.SetScale(...scale) &&
            mesh.SetMesh("meshes/cube.mesh") && mesh.material.SetType("builtin://materials/default-pbr") &&
            mesh.material.SetV4("baseColor", ...color, 1) && mesh.material.SetFloat("roughness", 0.65);
    }

    Inspect() {
        const light = this.scene?.FindNode("Sun")?.GetComponent(Light);
        return { method: light?.shadowMethod ?? null, settings: light?.GetVirtualShadowSettings() ?? null };
    }

    override OnShutdown(): void {
        this.scene = null;
        for (const resource of this.resources.splice(0)) {
            if (!Resources.Release(resource)) Debug.LogError("Cannot release virtual shadow resource");
        }
    }
}
