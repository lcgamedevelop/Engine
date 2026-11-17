import { UiController } from "engine";
import { MeshletModel } from "../models/meshlet_model.js";

export class MeshletController extends UiController<MeshletModel> {
    protected override OnBind(): boolean {
        const button = this.view.FindElement("meshlet-toggle");
        return !!button && this.Listen(button, "click", () => { this.model.toggle = true; });
    }

    protected override OnRefresh(): boolean {
        return !!this.view.FindElement("meshlet-toggle")?.SetText(
            `Scene Meshlets: ${this.model.enabled ? "ON" : "OFF"}`) &&
            !!this.view.FindElement("meshlet-status")?.SetText(this.model.status);
    }
}
