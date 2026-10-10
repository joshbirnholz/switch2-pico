// The profile list (pick, add) and the profile editor (name, emulated
// controller, button mapping, sticks, rumble, Mouse Mode, delete). Every
// change is saved right away, as in the panel.
import { DialogButton, Focusable, ModalRoot, showModal, SliderField, TextField, ToggleField } from "@decky/ui";
import { useEffect, useRef, useState } from "react";
import { OutputIcon, SlotIcon } from "./icons";
import { AllButtonsModal, confirm, PickerModal, SOFT_CSS, SoftButton } from "./modals";
import {
  allButtons, CTRL_GC, Extra, isJoyCon, MODE_NAMES, MODE_SHORT, mouseMode, outputText, Profile, profileForm, Settings,
  slotLabel,
} from "./model";

// Saves an urlencoded settings form; the dongle's settings after it, or null.
export type SaveFn = (form: string) => Promise<Settings | null>;

const row: React.CSSProperties = {
  display: "flex", alignItems: "center", gap: 12, width: "100%", minHeight: 52, padding: "8px 12px",
  textAlign: "left", boxSizing: "border-box",
};
const iconBox: React.CSSProperties = { flex: "none", minWidth: 40, display: "flex", justifyContent: "center" };
const sub: React.CSSProperties = { fontSize: 13, color: "#a3adba", marginTop: 2, lineHeight: 1.3 };
const backBtn: React.CSSProperties = { width: "auto", minWidth: 0, padding: "0 16px", height: 34 };

function Pencil() {
  return (
    <svg width="20" height="20" viewBox="0 0 20 20" aria-hidden="true">
      <path d="M4 13.5V16h2.5L14.6 7.9l-2.5-2.5L4 13.5zM16.3 6.2a.9.9 0 000-1.3l-1.2-1.2a.9.9 0 00-1.3 0l-1 1 2.5 2.5 1-1z" fill="#e6e9ed" />
    </svg>
  );
}

function Header(props: { title: string; subtitle?: string; onBack(): void }) {
  return (
    <>
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: 12 }}>
        <div style={{ fontSize: 22, fontWeight: 700 }}>{props.title}</div>
        <DialogButton style={backBtn} onClick={props.onBack}>Back</DialogButton>
      </div>
      {props.subtitle && <div style={{ ...sub, marginBottom: 12 }}>{props.subtitle}</div>}
    </>
  );
}

// The list of outputs for one button (`current` is the map as it is now).
export function pickOutput(S: Settings, P: Profile, e: Extra, current: number[], onPicked: (map: number[]) => void) {
  const mode = P.mode;
  const label = e.title ?? e.label;
  showModal(
    <PickerModal
      title={e.side ? `${label} · ${e.side}` : label}
      subtitle={`${P.name} · ${MODE_NAMES[mode]}`}
      items={S.modes[mode].outputs
        .map((name, i) => ({ name, i }))
        .filter(({ name }) => name !== null)
        .map(({ name, i }) => {
          const tx = outputText(name!);
          return { key: i, icon: <OutputIcon mode={mode} idx={i} />, title: tx.title, desc: tx.desc, selected: current[e.input] === i };
        })}
      onPick={(o) => {
        const map = current.slice();
        map[e.input] = o;
        onPicked(map);
      }}
    />,
  );
}

// A new profile in mode `mode`, with that mode's defaults (as the page makes one).
function newProfile(S: Settings, t: number, mode: number): Profile {
  return {
    name: MODE_SHORT[mode], mode, deadzone: 6, outer: 95, swap: 0, threshold: 120, rumble: 1, strength: 100,
    mouse: 0, mouse_speed: 100, mouse_flags: 0, map: S.types[t].defaults[mode].slice(),
  };
}

function pickMode(title: string, current: number | null, onPick: (mode: number) => void) {
  showModal(
    <PickerModal
      title={title}
      subtitle="Emulated controller"
      items={MODE_NAMES.map((name, m) => ({ key: m, icon: null, title: name, selected: m === current }))}
      onPick={onPick}
    />,
  );
}

// Profile: pick one (the row), edit one (the pencil), or add one.
export function ProfilesModal(props: { S: Settings; t: number; save: SaveFn; closeModal?(): void }) {
  const [S, setS] = useState(props.S);
  const t = props.t;
  const T = S.types[t];
  const save = async (form: string) => {
    const s = await props.save(form);
    if (s) setS(s);
    return s;
  };
  const free = T.profiles.map((p, i) => (p ? -1 : i)).filter((i) => i >= 0);
  const edit = (i: number) => showModal(<ProfileEditModal S={S} t={t} i={i} save={save} />);
  const add = () =>
    pickMode("New profile", null, (mode) => {
      const create = async (i: number) => {
        const s = await save(profileForm(t, i, newProfile(S, t, mode)));
        if (s) showModal(<ProfileEditModal S={s} t={t} i={i} save={save} />);
      };
      // Numbered profiles take the next one; the others go on a button.
      if (T.numbered) {
        create(free[0]);
        return;
      }
      showModal(
        <PickerModal
          title="New profile"
          subtitle="Its button (hold C + Home on the controller, then press it)"
          items={free.map((i) => ({ key: i, icon: <SlotIcon label={slotLabel(T, i)} gamecube={t === CTRL_GC} />, title: slotLabel(T, i) }))}
          onPick={create}
        />,
      );
    });
  return (
    <ModalRoot closeModal={props.closeModal}>
      <style>{SOFT_CSS}</style>
      <Header title="Profile" subtitle={T.name} onBack={() => props.closeModal?.()} />
      <Focusable flow-children="column" style={{ display: "flex", flexDirection: "column", gap: 4, maxHeight: "60vh", overflowY: "auto" }}>
        {T.profiles.map((p, i) =>
          p ? (
            <Focusable key={i} flow-children="row" style={{ display: "flex", gap: 4 }}>
              <SoftButton
                style={{ ...row, flex: 1, minWidth: 0 }}
                selected={i === T.active}
                onActivate={() => {
                  save(`active_${t}=${i}`);
                  props.closeModal?.();
                }}
              >
                <span style={iconBox}>
                  <SlotIcon label={slotLabel(T, i)} gamecube={t === CTRL_GC} />
                </span>
                <span style={{ flex: 1, minWidth: 0 }}>
                  <div style={{ fontWeight: 600 }}>{p.name}</div>
                  <div style={sub}>{MODE_NAMES[p.mode]}</div>
                </span>
                {i === T.active && (
                  <svg width="18" height="18" viewBox="0 0 18 18" aria-hidden="true">
                    <path d="M4 9.5l3.2 3L14 5.5" fill="none" stroke="#ffffff" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
                  </svg>
                )}
              </SoftButton>
              <SoftButton style={{ ...row, width: 52, flex: "none", justifyContent: "center", padding: 0 }} onActivate={() => edit(i)}>
                <Pencil />
              </SoftButton>
            </Focusable>
          ) : null,
        )}
        {free.length > 0 && (
          <DialogButton style={{ marginTop: 8 }} onClick={add}>
            Add a profile
          </DialogButton>
        )}
      </Focusable>
      {!T.numbered && <div style={{ ...sub, marginTop: 12 }}>On the controller: hold C + Home, then press the profile's button.</div>}
    </ModalRoot>
  );
}

// One profile's settings.
export function ProfileEditModal(props: { S: Settings; t: number; i: number; save: SaveFn; closeModal?(): void }) {
  const { t, i } = props;
  const [S, setS] = useState(props.S);
  const [P, setP] = useState<Profile>(props.S.types[t].profiles[i]!);
  const [name, setName] = useState(P.name);
  const nameTimer = useRef<ReturnType<typeof setTimeout>>();
  const sliderTimer = useRef<ReturnType<typeof setTimeout>>();
  const T = S.types[t];
  const count = T.profiles.filter((p) => p).length;

  const commit = async (np: Profile) => {
    setP(np);
    const s = await props.save(profileForm(t, i, np));
    if (s) setS(s);
  };
  // Sliders: saved once they rest.
  const slide = (np: Profile) => {
    setP(np);
    clearTimeout(sliderTimer.current);
    sliderTimer.current = setTimeout(() => commit(np), 400);
  };
  useEffect(() => () => {
    clearTimeout(nameTimer.current);
    clearTimeout(sliderTimer.current);
  }, []);

  const changeMode = () =>
    pickMode(P.name, P.mode, (mode) => {
      if (mode === P.mode) return;
      confirm(
        `Change to ${MODE_NAMES[mode]}?`,
        "Its button mapping will start over from that controller's defaults." +
          (i === T.active ? " The dongle will restart, and then you'll need to reconnect your controller." : ""),
        "Change",
        async () => {
          await commit({ ...P, mode, map: T.defaults[mode].slice() });
          // The profile in use: the dongle restarts now.
          if (i === T.active) props.closeModal?.();
        },
      );
    });

  const del = () =>
    confirm(
      `Delete ${P.name}?`,
      i === T.active ? "Another profile will take over for this controller." : "This can't be undone.",
      "Delete",
      async () => {
        await props.save(`prof_${t}_${i}=`);
        props.closeModal?.();
      },
      true,
    );

  return (
    <ModalRoot closeModal={props.closeModal}>
      <style>{SOFT_CSS}</style>
      <Header title={P.name || "Profile"} subtitle={`${T.name} · ${slotLabel(T, i)}`} onBack={() => props.closeModal?.()} />
      <Focusable flow-children="column" style={{ display: "flex", flexDirection: "column", gap: 4, maxHeight: "62vh", overflowY: "auto" }}>
        <TextField
          label="Name"
          value={name}
          onChange={(e) => {
            const v = e.target.value.slice(0, 19);
            setName(v);
            clearTimeout(nameTimer.current);
            if (v.trim()) nameTimer.current = setTimeout(() => commit({ ...P, name: v }), 600);
          }}
        />
        <SoftButton style={row} onActivate={changeMode}>
          <span style={{ flex: 1, minWidth: 0 }}>
            <div style={sub}>Emulated controller</div>
            <div style={{ fontWeight: 600 }}>{MODE_NAMES[P.mode]}</div>
          </span>
        </SoftButton>
        <SoftButton
          style={row}
          onActivate={() =>
            showModal(
              <AllButtonsModal
                title="Button mapping"
                subtitle={`${P.name} · ${MODE_NAMES[P.mode]}`}
                buttons={allButtons(t)}
                map={P.map}
                mode={P.mode}
                onPick={(e, current, done) =>
                  pickOutput(S, P, e, current, (map) => {
                    done(map);
                    commit({ ...P, map });
                  })
                }
              />,
            )
          }
        >
          <span style={{ flex: 1, minWidth: 0, fontWeight: 600 }}>Button mapping</span>
        </SoftButton>
        <SliderField label="Stick deadzone" value={P.deadzone} min={0} max={40} step={1} showValue valueSuffix="%"
          onChange={(v) => slide({ ...P, deadzone: v })} />
        <SliderField label="Full deflection at" value={P.outer} min={50} max={100} step={1} showValue valueSuffix="%"
          onChange={(v) => slide({ ...P, outer: v })} />
        <ToggleField label="Swap sticks" checked={!!P.swap} onChange={(v) => commit({ ...P, swap: v ? 1 : 0 })} />
        {t === CTRL_GC && (
          <SliderField label="L / R press threshold" value={P.threshold} min={0} max={255} step={5} showValue
            onChange={(v) => slide({ ...P, threshold: v })} />
        )}
        <ToggleField label="Rumble" checked={!!P.rumble} onChange={(v) => commit({ ...P, rumble: v ? 1 : 0 })} />
        {!!P.rumble && (
          <SliderField label="Rumble strength" value={P.strength} min={0} max={200} step={10} showValue valueSuffix="%"
            onChange={(v) => slide({ ...P, strength: v })} />
        )}
        {isJoyCon(t) && mouseMode(P.mode) && (
          <>
            <ToggleField label="Mouse Mode" description="Lay a Joy-Con on its side to use it as a mouse."
              checked={!!P.mouse} onChange={(v) => commit({ ...P, mouse: v ? 1 : 0, mouse_speed: P.mouse_speed || 100 })} />
            {!!P.mouse && (
              <SliderField label="Mouse speed" value={P.mouse_speed || 100} min={10} max={250} step={10} showValue valueSuffix="%"
                onChange={(v) => slide({ ...P, mouse_speed: v })} />
            )}
          </>
        )}
        <DialogButton style={{ marginTop: 12 }} disabled={count <= 1} onClick={del}>
          Delete profile…
        </DialogButton>
      </Focusable>
    </ModalRoot>
  );
}
