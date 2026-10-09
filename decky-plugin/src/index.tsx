// Switch2-Pico: quick controls for the dongle in the Quick Access menu.
import { addEventListener, definePlugin, removeEventListener, toaster } from "@decky/api";
import { ButtonItem, PanelSection, PanelSectionRow, showModal, SliderField, staticClasses, ToggleField } from "@decky/ui";
import { useEffect, useRef, useState } from "react";
import { action, checkUpdate, getPrefs, getSettings, getStatus, Prefs, setPref, setSettings, UpdateInfo } from "./api";
import { InputIcon, OutputIcon, SlotIcon } from "./icons";
import {
  CTRL_GC, CtrlType, extraButtons, isJoyCon, MF_INVERT_H, MF_INVERT_V, MF_UP_DOWN_ONLY, MODE_NAMES, MODE_X360, outputText, PIDS,
  Profile, profileForm, quickRemap, Settings, slotLabel, Status, STATUS_MODES,
} from "./model";
import { confirm, PairedModal, PickerModal, UpdateModal } from "./modals";

const hint: React.CSSProperties = { fontSize: 12.5, color: "#a3adba", lineHeight: 1.35 };

function Battery({ pct, label }: { pct: number; label?: string }) {
  const w = Math.max(1, Math.round((11 * pct) / 100));
  return (
    <span style={{ display: "inline-flex", alignItems: "center", gap: 4 }}>
      <svg width="18" height="10" viewBox="0 0 18 10" aria-hidden="true">
        <rect x="0.5" y="0.5" width="15" height="9" rx="2" fill="none" stroke="#a3adba" />
        <rect x="2" y="2" width={w + 0.5} height="6" rx="1" fill={pct <= 20 ? "#ff8a7a" : "#e6e9ed"} />
        <rect x="16" y="3" width="1.5" height="4" rx="0.5" fill="#a3adba" />
      </svg>
      {label ? `${label} ${pct}%` : `${pct}%`}
    </span>
  );
}

const sideLetter = (pid: number) => (pid === 0x2067 ? "L" : pid === 0x2066 ? "R" : undefined);

function Content() {
  const [st, setSt] = useState<Status | null>(null);
  const [err, setErr] = useState("");
  const [S, setS] = useState<Settings | null>(null);
  const [prefs, setPrefs] = useState<Prefs | null>(null);
  const [upd, setUpd] = useState<UpdateInfo | null>(null);
  const [later, setLater] = useState(false);
  const [restarting, setRestarting] = useState(false);
  const typeSeen = useRef(-1);
  const speedTimer = useRef<ReturnType<typeof setTimeout>>();
  const [speed, setSpeed] = useState<number | null>(null);   // slider while it moves

  const loadSettings = async () => {
    const r = await getSettings();
    if (r.ok && r.settings) setS(r.settings);
  };
  const poll = async () => {
    const r = await getStatus();
    if (r.ok && r.status) {
      setSt(r.status);
      setErr("");
      setRestarting(false);
      // Controller type changed (or the dongle came back): settings again.
      if (r.status.ctrl_type !== typeSeen.current) {
        typeSeen.current = r.status.ctrl_type;
        loadSettings();
      }
    } else {
      setSt(null);
      setErr(r.error ?? "");
      typeSeen.current = -1;
    }
  };
  useEffect(() => {
    poll();
    getPrefs().then(setPrefs);
    checkUpdate(false).then(setUpd);
    const id = setInterval(poll, 1000);
    return () => clearInterval(id);
  }, []);

  // Settings changes; a change of USB mode (or Mouse Mode) restarts the dongle.
  const save = async (form: string, restarts = false) => {
    if (restarts) setRestarting(true);
    const r = await setSettings(form);
    if (r.ok && r.settings) setS(r.settings);
    else if (!restarts) toaster.toast({ title: "Switch2-Pico", body: `Not saved: ${r.error}` });
    if (restarts) typeSeen.current = -1;
  };

  if (!st) {
    const busy = err.startsWith("in use");
    return (
      <PanelSection>
        <PanelSectionRow>
          <div style={{ textAlign: "center", padding: "16px 0 8px" }}>
            <div style={{ fontSize: 17, fontWeight: 600, marginBottom: 6 }}>
              {restarting ? "Dongle restarting…" : busy ? "Dongle busy" : "Dongle not found"}
            </div>
            <div style={hint}>
              {restarting
                ? "It reconnects in a few seconds."
                : busy
                  ? "Another app (the configuration page in a browser) is using it. Close that and try again."
                  : "Plug the Switch2-Pico dongle into the Steam Deck or dock. With “Only appear on USB while a controller is connected” on, it hides until a controller connects: press a button on your controller."}
            </div>
          </div>
        </PanelSectionRow>
        {!restarting && (
          <PanelSectionRow>
            <ButtonItem layout="below" onClick={poll}>Try again</ButtonItem>
          </PanelSectionRow>
        )}
      </PanelSection>
    );
  }

  const t = st.ctrl_type;
  const T: CtrlType | undefined = S?.types[t];
  const ai = T?.active ?? 0;
  const P: Profile | null | undefined = T?.profiles[ai];
  const mode = P ? P.mode : STATUS_MODES[st.usb_mode] ?? 0;
  const ready = st.links.filter((k) => k.state === "ready");
  const runMode = STATUS_MODES[st.usb_mode] ?? 0;

  const saveProfile = (np: Profile, restarts = false) => save(profileForm(t, ai, np), restarts);

  const pickProfile = () => {
    if (!T) return;
    showModal(
      <PickerModal
        title="Profile"
        subtitle={T.name}
        items={T.profiles
          .map((p, i) => ({ p, i }))
          .filter(({ p }) => p)
          .map(({ p, i }) => ({
            key: i,
            icon: <SlotIcon label={slotLabel(T, i)} gamecube={t === CTRL_GC} />,
            title: p!.name,
            desc: MODE_NAMES[p!.mode],
            selected: i === ai,
          }))}
        footer={T.numbered ? undefined : "On the controller: hold C + Home, then press the profile's button."}
        onPick={(i) => {
          const np = T.profiles[i]!;
          const restarts = np.mode !== runMode || (isJoyCon(t) && (np.mode === MODE_X360 && np.mouse ? 1 : 0) !== (st.usb_mouse ? 1 : 0));
          save(`active_${t}=${i}`, restarts);
        }}
      />,
    );
  };

  const pickOutput = (input: number, label: string, side?: string) => {
    if (!S || !P) return;
    const outs = S.modes[mode].outputs;
    showModal(
      <PickerModal
        title={side ? `${label} · ${side}` : label}
        subtitle={`${P.name} · ${MODE_NAMES[mode]}`}
        items={outs
          .map((name, i) => ({ name, i }))
          .filter(({ name }) => name !== null)
          .map(({ name, i }) => {
            const tx = outputText(name!);
            return { key: i, icon: <OutputIcon mode={mode} idx={i} />, title: tx.title, desc: tx.desc, selected: P.map[input] === i };
          })}
        onPick={(o) => {
          const map = P.map.slice();
          map[input] = o;
          saveProfile({ ...P, map });
        }}
      />,
    );
  };

  const remap = quickRemap(t, mode);
  const mouseOK = mode === MODE_X360;
  const flags = P?.mouse_flags ?? 0;
  const setFlag = (bit: number, on: boolean) => P && saveProfile({ ...P, mouse_flags: on ? flags | bit : flags & ~bit });
  const pairing = st.pairing;

  return (
    <>
      <PanelSection>
        <PanelSectionRow>
          <div style={{ padding: 12, borderRadius: 6, background: "#1f252e", display: "flex", flexDirection: "column", gap: 6 }}>
            <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", gap: 8 }}>
              <span style={{ display: "flex", alignItems: "center", gap: 8, fontWeight: 600 }}>
                <span style={{ width: 8, height: 8, borderRadius: 4, background: ready.length ? "#3ecf6e" : "#5c6470", flex: "none" }} />
                {ready.length ? T?.name ?? PIDS[ready[0].pid] : "No controller connected"}
              </span>
              <span style={{ display: "flex", gap: 10, fontSize: 14 }}>
                {ready.map((k) => (
                  <Battery key={k.addr} pct={k.battery_pct} label={ready.length > 1 ? sideLetter(k.pid) : undefined} />
                ))}
              </span>
            </div>
            <div style={hint}>{MODE_NAMES[runMode]}</div>
          </div>
        </PanelSectionRow>
        {upd?.available && !later && (
          <PanelSectionRow>
            <div style={{ padding: 12, borderRadius: 6, background: "#2e2611", display: "flex", flexDirection: "column", gap: 8 }}>
              <div style={{ fontWeight: 600, color: "#f5c35a" }}>Firmware {upd.latest} is available</div>
              <div style={{ display: "flex", gap: 8 }}>
                <ButtonItem
                  layout="below"
                  onClick={() => showModal(<UpdateModal version={upd.latest!} onDone={() => checkUpdate(true).then(setUpd)} />)}
                >
                  Update now
                </ButtonItem>
                <ButtonItem layout="below" onClick={() => setLater(true)}>Later</ButtonItem>
              </div>
            </div>
          </PanelSectionRow>
        )}
      </PanelSection>

      <PanelSection title="Controller">
        {pairing.required && (
          <PanelSectionRow>
            <ButtonItem layout="below" onClick={() => action("pair").then(poll)}>
              {pairing.open ? `Stop pairing (${Math.ceil(pairing.left_ms / 1000)} s)` : "Pair a controller"}
            </ButtonItem>
          </PanelSectionRow>
        )}
        {ready.length > 0 && (
          <PanelSectionRow>
            <ButtonItem layout="below" onClick={() => action("disconnect").then(poll)}>Disconnect</ButtonItem>
          </PanelSectionRow>
        )}
        {T && P && (
          <PanelSectionRow>
            <ButtonItem layout="inline" label="Profile" onClick={pickProfile}>
              <span style={{ display: "inline-flex", alignItems: "center", gap: 8 }}>
                {!T.numbered && <SlotIcon label={slotLabel(T, ai)} gamecube={t === CTRL_GC} />}
                {T.numbered ? `${ai + 1} · ${P.name}` : P.name}
              </span>
            </ButtonItem>
          </PanelSectionRow>
        )}
      </PanelSection>

      {T && P && S && (
        <PanelSection title="Extra buttons">
          {extraButtons(t).map((e) => {
            const o = P.map[e.input];
            return (
              <PanelSectionRow key={e.input}>
                <ButtonItem
                  layout="inline"
                  label={
                    <span style={{ display: "inline-flex", alignItems: "center", gap: 8 }}>
                      <InputIcon label={e.label} />
                      {e.side && <span style={hint}>{e.side}</span>}
                    </span>
                  }
                  onClick={() => pickOutput(e.input, e.label, e.side)}
                >
                  <span style={{ display: "inline-flex", alignItems: "center", gap: 6, maxWidth: 150, overflow: "hidden" }}>
                    <OutputIcon mode={mode} idx={o} />
                  </span>
                </ButtonItem>
              </PanelSectionRow>
            );
          })}
          {remap && (
            <PanelSectionRow>
              <ToggleField
                label={`Remap ${remap} from the controller`}
                description={`Hold C + ${remap.split("/")[0]} or ${remap.split("/")[1]}, then press a button`}
                checked={!!S.quick_remap}
                onChange={(v) => save(`quick_remap=${v ? 1 : 0}`)}
              />
            </PanelSectionRow>
          )}
        </PanelSection>
      )}

      {T && P && isJoyCon(t) && (
        <PanelSection title="Mouse Mode">
          <PanelSectionRow>
            <ToggleField
              label="Mouse Mode"
              description={
                mouseOK
                  ? "Lay a Joy-Con on its side to use it as a mouse. Turning it on or off restarts the dongle."
                  : "Only available for profiles that emulate an Xbox 360 controller."
              }
              disabled={!mouseOK}
              checked={mouseOK && !!P.mouse}
              onChange={(v) => saveProfile({ ...P, mouse: v ? 1 : 0, mouse_speed: P.mouse_speed || 100 }, true)}
            />
          </PanelSectionRow>
          {mouseOK && !!P.mouse && (
            <>
              <PanelSectionRow>
                <SliderField
                  label="Speed"
                  value={speed ?? (P.mouse_speed || 100)}
                  min={10}
                  max={250}
                  step={10}
                  showValue
                  valueSuffix="%"
                  onChange={(v) => {
                    // Saved once the slider rests.
                    setSpeed(v);
                    clearTimeout(speedTimer.current);
                    speedTimer.current = setTimeout(() => saveProfile({ ...P, mouse_speed: v }).then(() => setSpeed(null)), 400);
                  }}
                />
              </PanelSectionRow>
              <PanelSectionRow>
                <ToggleField label="Scroll sideways with left / right" checked={!(flags & MF_UP_DOWN_ONLY)} onChange={(v) => setFlag(MF_UP_DOWN_ONLY, !v)} />
              </PanelSectionRow>
              <PanelSectionRow>
                <ToggleField label="Invert up / down scrolling" checked={!!(flags & MF_INVERT_V)} onChange={(v) => setFlag(MF_INVERT_V, v)} />
              </PanelSectionRow>
              <PanelSectionRow>
                <ToggleField label="Invert left / right scrolling" checked={!!(flags & MF_INVERT_H)} onChange={(v) => setFlag(MF_INVERT_H, v)} />
              </PanelSectionRow>
            </>
          )}
        </PanelSection>
      )}

      <PanelSection title="Dongle">
        <PanelSectionRow>
          <ButtonItem
            layout="inline"
            label="Paired controllers"
            description={`${st.bond.list.length} remembered`}
            onClick={() => showModal(<PairedModal onChange={poll} />)}
          >
            Manage
          </ButtonItem>
        </PanelSectionRow>
        {prefs && (
          <PanelSectionRow>
            <ToggleField
              label="Check for updates"
              description="On start and once a day"
              checked={prefs.check_updates}
              onChange={(v) => setPref("check_updates", v).then(setPrefs)}
            />
          </PanelSectionRow>
        )}
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            onClick={() =>
              confirm("Reboot the dongle?", "The controller reconnects by itself after a few seconds.", "Reboot", () => {
                setRestarting(true);
                action("reboot");
              })
            }
          >
            Reboot dongle
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <div style={{ ...hint, paddingTop: 4 }}>
            Firmware {st.version} ({st.platform})
            {upd?.ok && !upd.available && upd.latest ? " · up to date" : ""}
          </div>
        </PanelSectionRow>
      </PanelSection>
    </>
  );
}

// Toasts for the backend's "notify" events (also while the menu is closed).
async function onNotify(kind: string, a: string | number, b: string | number) {
  if (kind === "connect") {
    const r = await getStatus();
    if (!r.ok || !r.status) return;
    const s = r.status;
    const ready = s.links.filter((k) => k.state === "ready");
    if (!ready.length) return;
    const pair = ready.length > 1;
    toaster.toast({
      title: pair ? "Joy-Con 2 (L/R) connected" : `${PIDS[ready[0].pid] ?? "Controller"} connected`,
      body: ready.map((k) => (pair ? `${sideLetter(k.pid)} ${k.battery_pct}%` : `${k.battery_pct}%`)).join(" · ") + ` · ${s.profile}`,
    });
  } else if (kind === "battery") {
    toaster.toast({ title: `${PIDS[Number(a)] ?? "Controller"} battery low`, body: `${b}% left` });
  } else if (kind === "update") {
    toaster.toast({ title: `Switch2-Pico ${a} available`, body: "Open the plugin to update" });
  } else if (kind === "profile") {
    toaster.toast({ title: `Profile: ${a}`, body: MODE_NAMES[STATUS_MODES[String(b)] ?? 0] });
  }
}

function PluginIcon() {
  return (
    <svg width="1em" height="1em" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
      <rect x="3" y="7" width="18" height="10" rx="3" />
      <path d="M7 12h3M8.5 10.5v3" />
      <circle cx="16" cy="11" r="0.8" />
      <circle cx="17.5" cy="13" r="0.8" />
    </svg>
  );
}

export default definePlugin(() => {
  const listener = addEventListener<[string, string | number, string | number]>("notify", onNotify);
  return {
    name: "Switch2-Pico",
    titleView: <div className={staticClasses.Title}>Switch2-Pico</div>,
    content: <Content />,
    icon: <PluginIcon />,
    onDismount() {
      removeEventListener("notify", listener);
    },
  };
});
