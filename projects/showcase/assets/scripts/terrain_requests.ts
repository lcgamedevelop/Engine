import { TerrainComponent, TerrainOperationId, TerrainOperationSnapshot, TerrainBounds } from "engine";

// Project-side sequencing only. Native requests own preparation, publication and cancellation.
export class TerrainRequests {
    private pending: { terrain: TerrainComponent; id: TerrainOperationId;
        complete: (result: TerrainOperationSnapshot | null) => void }[] = [];
    error = "";
    get busy(): boolean { return this.pending.length > 0; }
    Watch(terrain: TerrainComponent, id: TerrainOperationId | null,
          complete: (result: TerrainOperationSnapshot | null) => void = () => {}): boolean {
        if (!id) { this.error = "Terrain request rejected"; return false; }
        this.pending.push({terrain,id,complete});
        return true;
    }
    Update(): void {
        const batch = this.pending.splice(0);
        for (const item of batch) {
            const result = item.terrain.GetOperation(item.id);
            if (result && !["completed", "cancelled", "failed"].includes(result.state)) {
                this.pending.push(item); continue;
            }
            if (result && !item.terrain.ReleaseOperation(item.id)) {
                this.pending.push(item); continue;
            }
            if (result?.state !== "completed") this.error = result?.error || "Terrain owner was released";
            item.complete(result);
        }
    }
    Shutdown(): void {
        for (const item of this.pending) {
            item.terrain.CancelOperation(item.id);
            item.terrain.ReleaseOperation(item.id);
        }
        this.pending.length = 0;
    }
}

export function terrainReady(terrain: TerrainComponent | null, bounds: TerrainBounds): boolean {
    const state = terrain?.GetRegions({bounds,limit:4096});
    return !!state && state.revision > 0n && !state.nextCursor && state.regions.length > 0 &&
        state.regions.every(region => region.known && region.state === "ready" &&
            region.collisionRequired && region.densityRevision === region.collisionRevision);
}
