import { UiController } from "engine";
import { TerrainWorldModel, world_stops } from "../models/terrain_world_model.js";

export class TerrainWorldController extends UiController<TerrainWorldModel> {
    protected override OnBind(): boolean {
        return world_stops.every((_stop, index) => {
            const button = this.view.FindElement(`world-${index}`);
            return !!button && this.Listen(button, "click", () => { this.model.pending = index; });
        });
    }
    protected override OnRefresh(): boolean {
        return !!this.view.FindElement("world-status")?.SetText(this.model.status) &&
            world_stops.every((_stop, index) =>
                !!this.view.FindElement(`world-${index}`)?.SetEnabled(!this.model.busy));
    }
}
