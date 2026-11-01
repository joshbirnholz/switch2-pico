import { Dropdown, Field, SliderField, ToggleField } from "@decky/ui";
import { Profile } from "./model";

export function RumbleOptions({ p, save }: { p: Profile; save(p: Profile): void }) {
  const flags = p.impulse_flags || 0;
  const dual = p.mode === 1 || p.mode === 2;
  return <>
    <Field label="Rumble style">
      <Dropdown selectedOption={p.rumble_style || 0} rgOptions={[
        { data: 0, label: "Stereo motors (original)" }, { data: 1, label: "Xbox (traditional motors)" },
        { data: 2, label: "Switch HD Rumble" }, ...(dual ? [{ data: 3, label: "DualSense audio haptics" }] : []),
      ]} onChange={(v) => save({ ...p, rumble_style: v.data })} />
    </Field>
    {p.rumble_style === 1 && <SliderField label="Xbox frequency" min={1} max={10} step={1} showValue
      value={p.rumble_frequency || 5} onChange={(v) => save({ ...p, rumble_frequency: v })} />}
    {dual && <>
      <ToggleField label="Trigger feedback" description="Supported adaptive trigger effects will become vibration."
        checked={!(flags & 1)} onChange={(v) => save({ ...p, impulse_flags: v ? flags & ~1 : flags | 1 })} />
      {!(flags & 1) && <>
        <ToggleField label="Dynamic trigger frequency" checked={!(flags & 2)}
          onChange={(v) => save({ ...p, impulse_flags: v ? flags & ~2 : flags | 2 })} />
        <SliderField label="Trigger strength" value={p.impulse_strength ?? 100} min={0} max={200} step={10} showValue valueSuffix="%"
          onChange={(v) => save({ ...p, impulse_strength: v })} />
        {!!(flags & 2) && <SliderField label="Trigger frequency" value={p.impulse_frequency || 10} min={1} max={10} step={1} showValue
          onChange={(v) => save({ ...p, impulse_frequency: v })} />}
      </>}
      {p.rumble_style === 3 && <div style={{ fontSize: 13 }}>Audio haptics require a Pico 2 W build with audio enabled and a compatible game using playback channels 3/4.</div>}
    </>}
  </>;
}
