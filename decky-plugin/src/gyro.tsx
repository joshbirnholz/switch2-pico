// Gyro mouse options of a Joy-Con 2 profile (the panel's "Gyro mouse"
// section and the profile editor): on / off, speed, axes, mouse buttons, and
// the button combinations that turn it on and off and recenter it.
import { DialogButton, Focusable, ModalRoot, showModal, SliderField, ToggleField } from "@decky/ui";
import { useRef, useState } from "react";
import { InputIcon } from "./icons";
import { SOFT_CSS, SoftButton } from "./modals";
import {
  allButtons, comboText, CTRL_PAIR, GMF_INVERT_X, GMF_INVERT_Y, GMF_NO_CLICKS, gyroToggleDefault, mouseMode, Profile,
} from "./model";

const row: React.CSSProperties = {
  display: "flex", alignItems: "center", gap: 12, width: "100%", minHeight: 48, padding: "6px 12px",
  textAlign: "left", boxSizing: "border-box",
};
const sub: React.CSSProperties = { fontSize: 13, color: "#a3adba", marginTop: 2, lineHeight: 1.3 };
const small: React.CSSProperties = { width: "auto", minWidth: 0, padding: "0 16px", height: 34 };

// Pick the buttons of a combination: each one on or off. The choice applies
// on leaving (Done, or Back on the controller).
function ComboModal(props: {
  title: string; description: string; t: number; bits: number; onSave(bits: number): void; closeModal?(): void;
}) {
  const [bits, setBits] = useState(props.bits);
  const done = () => {
    if (bits !== props.bits) props.onSave(bits >>> 0);
    props.closeModal?.();
  };
  return (
    <ModalRoot closeModal={done}>
      <style>{SOFT_CSS}</style>
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: 12 }}>
        <div style={{ fontSize: 22, fontWeight: 700 }}>{props.title}</div>
        <Focusable style={{ display: "flex", gap: 8 }} flow-children="row">
          <DialogButton style={small} onClick={() => setBits(0)}>Clear</DialogButton>
          <DialogButton style={small} onClick={done}>Done</DialogButton>
        </Focusable>
      </div>
      <div style={{ ...sub, marginBottom: 12 }}>{props.description}</div>
      <Focusable flow-children="column" style={{ display: "flex", flexDirection: "column", gap: 4, maxHeight: "55vh", overflowY: "auto" }}>
        {allButtons(props.t).map((e) => {
          const on = !!(bits & (1 << e.input));
          return (
            <SoftButton key={e.input} style={row} selected={on} onActivate={() => setBits(on ? bits & ~(1 << e.input) : bits | (1 << e.input))}>
              <span style={{ flex: "none", minWidth: 40, display: "flex", justifyContent: "center" }}><InputIcon label={e.label} /></span>
              <span style={{ flex: 1, minWidth: 0 }}>
                <div style={{ fontWeight: 600 }}>{e.title ?? e.label}</div>
                {e.side && <div style={sub}>{e.side}</div>}
              </span>
              {on && (
                <svg width="18" height="18" viewBox="0 0 18 18" aria-hidden="true">
                  <path d="M4 9.5l3.2 3L14 5.5" fill="none" stroke="#ffffff" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
                </svg>
              )}
            </SoftButton>
          );
        })}
      </Focusable>
      <div style={{ ...sub, marginTop: 12 }}>Selected: {comboText(props.t, bits)}</div>
    </ModalRoot>
  );
}

// `wrap` puts each control in the caller's row (PanelSectionRow in the panel).
export function GyroMouseOptions(props: { t: number; p: Profile; save(p: Profile): void; wrap?(c: JSX.Element, key: string): JSX.Element }) {
  const { t, p } = props;
  const wrap = props.wrap ?? ((c: JSX.Element) => c);
  const timer = useRef<ReturnType<typeof setTimeout>>();
  const [speed, setSpeed] = useState<number | null>(null);   // slider while it moves
  if (p.gyro_mouse === undefined) {
    return wrap(<div style={sub}>The gyro mouse needs firmware 0.21.0 or newer.</div>, "gm-update");
  }
  const ok = mouseMode(p.mode);
  const flags = p.gyro_mouse_flags ?? 0;
  const setFlag = (bit: number, on: boolean) => props.save({ ...p, gyro_mouse_flags: on ? flags | bit : flags & ~bit });
  const toggle = p.gyro_mouse_toggle ?? gyroToggleDefault(t);
  const recenter = p.gyro_mouse_recenter ?? 0;
  const which = t === CTRL_PAIR ? `${p.gyro_side ? "left" : "right"} Joy-Con` : "Joy-Con";
  const pick = (title: string, description: string, bits: number, key: "gyro_mouse_toggle" | "gyro_mouse_recenter") =>
    showModal(<ComboModal title={title} description={description} t={t} bits={bits} onSave={(b) => props.save({ ...p, [key]: b })} />);
  const comboRow = (label: string, bits: number, onActivate: () => void, key: string) =>
    wrap(
      <SoftButton style={row} onActivate={onActivate}>
        <span style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontWeight: 600 }}>{label}</div>
          <div style={sub}>{comboText(t, bits)}</div>
        </span>
      </SoftButton>,
      key,
    );
  return (
    <>
      {wrap(
        <ToggleField
          label="Gyro mouse"
          description={ok ? `Turn and tilt the ${which} to move the pointer.` : "Only available for profiles that emulate an Xbox 360 or SInput controller."}
          disabled={!ok}
          checked={ok && !!p.gyro_mouse}
          onChange={(v) => props.save({ ...p, gyro_mouse: v ? 1 : 0, gyro_mouse_speed: p.gyro_mouse_speed || 100 })}
        />,
        "gm-on",
      )}
      {ok && !!p.gyro_mouse && (
        <>
          {wrap(
            <SliderField
              label="Gyro speed"
              value={speed ?? (p.gyro_mouse_speed || 100)}
              min={10}
              max={250}
              step={10}
              showValue
              valueSuffix="%"
              onChange={(v) => {
                // Saved once the slider rests.
                setSpeed(v);
                clearTimeout(timer.current);
                timer.current = setTimeout(() => { props.save({ ...p, gyro_mouse_speed: v }); setSpeed(null); }, 400);
              }}
            />,
            "gm-speed",
          )}
          {comboRow("On / off buttons", toggle, () =>
            pick("On / off buttons", "Press these together to turn the gyro mouse on or off. With none, it will always be on.", toggle, "gyro_mouse_toggle"),
          "gm-toggle")}
          {comboRow("Recenter buttons", recenter, () =>
            pick("Recenter buttons", "Hold these together to keep the pointer still while you bring the Joy-Con back to center.", recenter, "gyro_mouse_recenter"),
          "gm-recenter")}
          {wrap(
            <ToggleField
              label="Mouse buttons on the Joy-Con"
              description="While it's on, the shoulder button will click, the trigger will right-click and the stick click will middle-click."
              checked={!(flags & GMF_NO_CLICKS)}
              onChange={(v) => setFlag(GMF_NO_CLICKS, !v)}
            />,
            "gm-clicks",
          )}
          {wrap(<ToggleField label="Invert left/right" checked={!!(flags & GMF_INVERT_X)} onChange={(v) => setFlag(GMF_INVERT_X, v)} />, "gm-inv-x")}
          {wrap(<ToggleField label="Invert up/down" checked={!!(flags & GMF_INVERT_Y)} onChange={(v) => setFlag(GMF_INVERT_Y, v)} />, "gm-inv-y")}
        </>
      )}
    </>
  );
}
