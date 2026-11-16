export const world_stops = [
    { name: "Southwest", x: -3600, z: -3600 },
    { name: "Southeast", x: 3600, z: -3600 },
    { name: "Northeast", x: 3600, z: 3600 },
    { name: "Northwest", x: -3600, z: 3600 },
] as const;

export class TerrainWorldModel {
    pending: number | null = null;
    busy = false;
    status = "8 x 8 km / streamed volume / 16 m base samples";
}
