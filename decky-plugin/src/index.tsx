// Switch2-Pico: quick controls for the dongle in the Quick Access menu.
import { addEventListener, definePlugin, removeEventListener, toaster } from "@decky/api";
import {
  ButtonItem, DialogButton, Field, Focusable, PanelSection, PanelSectionRow, showModal, SliderField, staticClasses, ToggleField,
} from "@decky/ui";
import { CSSProperties, ReactNode, useEffect, useRef, useState } from "react";
import {
  action, checkPluginUpdate, checkUpdate, getPrefs, getSettings, getStatus, installPluginUpdate, PluginUpdate, Prefs, setPref,
  setSettings, UpdateInfo,
} from "./api";
import { InputIcon, OutputIcon, SlotIcon } from "./icons";
import {
  CTRL_GC, CtrlType, extraButtons, isJoyCon, MF_INVERT_H, MF_INVERT_V, MF_UP_DOWN_ONLY, MODE_NAMES, MODE_X360, outputText, PIDS,
  Profile, profileForm, quickRemap, Settings, slotLabel, Status, STATUS_MODES,
} from "./model";
import { confirm, PairedModal, PickerModal, SOFT_CSS, SoftButton, UpdateModal } from "./modals";

const hint: CSSProperties = { fontSize: 12.5, color: "#a3adba", lineHeight: 1.35 };
// Everything stays within the panel's width (no horizontal scrolling).
const card: CSSProperties = {
  boxSizing: "border-box", width: "100%", maxWidth: "100%", overflow: "hidden", padding: 10, borderRadius: 6,
  display: "flex", flexDirection: "column", gap: 8,
};
const btnRow: CSSProperties = { display: "flex", gap: 8, width: "100%", boxSizing: "border-box" };
const half: CSSProperties = { flex: 1, minWidth: 0, width: "auto", height: 36, padding: "0 8px", boxSizing: "border-box" };
const small: CSSProperties = {
  width: "auto", minWidth: 0, maxWidth: 160, height: 38, padding: "0 10px", boxSizing: "border-box",
  display: "flex", alignItems: "center", justifyContent: "center", gap: 6,
};
const ellipsis: CSSProperties = { overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap", minWidth: 0 };

// A label on the left and a compact control on the right.
function Row(props: { label: ReactNode; description?: ReactNode; children: ReactNode }) {
  return (
    <Field label={props.label} description={props.description} childrenContainerWidth="min">
      {props.children}
    </Field>
  );
}

// Custom content in a row, with the same side padding as Decky's own rows.
function Block(props: { children: ReactNode; separator?: boolean }) {
  return (
    <Field childrenLayout="below" childrenContainerWidth="max" bottomSeparator={props.separator ? "standard" : "none"}>
      {props.children}
    </Field>
  );
}

function Banner(props: { text: string; action: string; onAction(): void; onLater(): void }) {
  return (
    <Block>
    <div style={{ ...card, background: "#2e2611" }}>
      <div style={{ fontWeight: 600, color: "#f5c35a" }}>{props.text}</div>
      <Focusable style={btnRow}>
        <DialogButton style={half} onClick={props.onAction}>{props.action}</DialogButton>
        <DialogButton style={half} onClick={props.onLater}>Later</DialogButton>
      </Focusable>
    </div>
    </Block>
  );
}

// The icon only: the level is coarse (the controller's own 0–9).
function Battery({ pct, label }: { pct: number; label?: string }) {
  const w = Math.max(1, Math.round((11 * pct) / 100));
  return (
    <span style={{ display: "inline-flex", alignItems: "center", gap: 4 }}>
      <svg width="18" height="10" viewBox="0 0 18 10" aria-hidden="true">
        <rect x="0.5" y="0.5" width="15" height="9" rx="2" fill="none" stroke="#a3adba" />
        <rect x="2" y="2" width={w + 0.5} height="6" rx="1" fill={pct <= 20 ? "#ff8a7a" : "#e6e9ed"} />
        <rect x="16" y="3" width="1.5" height="4" rx="0.5" fill="#a3adba" />
      </svg>
      {label}
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
  const [pupd, setPupd] = useState<PluginUpdate | null>(null);
  const [pLater, setPLater] = useState(false);
  const [checking, setChecking] = useState(false);
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
    checkPluginUpdate(false).then(setPupd);
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

  const pluginBanner = pupd?.available && !pLater && (
    <PanelSectionRow>
      <Banner
        text={`Plugin ${pupd.latest} is available`}
        action="Update"
        onLater={() => setPLater(true)}
        onAction={() => updatePlugin(pupd)}
      />
    </PanelSectionRow>
  );

  if (!st) {
    const busy = err.startsWith("in use");
    return (
      <PanelSection>
        {pluginBanner}
        <PanelSectionRow>
          <Block>
          <div style={{ textAlign: "center", padding: "8px 0", boxSizing: "border-box", width: "100%" }}>
            <div style={{ fontSize: 17, fontWeight: 600, marginBottom: 6 }}>
              {restarting ? "Dongle restarting…" : busy ? "Dongle busy" : "Dongle not found"}
            </div>
            <div style={hint}>
              {restarting
                ? "It will reconnect in a few seconds."
                : busy
                  ? "Another app (the configuration page in a browser) is using it. Close that and try again."
                  : "Plug the Switch2-Pico dongle into the Steam Deck or dock. With “Only appear on USB while a controller is connected” on, it hides until a controller connects: press a button on your controller."}
            </div>
          </div>
          </Block>
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
      <style>{SOFT_CSS}</style>
      <PanelSection>
        {/* First: something focusable at the very top, so moving up with the
            D-pad scrolls the panel back to its top. */}
        {T && P && (
          <PanelSectionRow>
            <Row label="Profile">
              <SoftButton style={small} onActivate={pickProfile}>
                {!T.numbered && <SlotIcon label={slotLabel(T, ai)} gamecube={t === CTRL_GC} />}
                <span style={ellipsis}>{T.numbered ? `${ai + 1} · ${P.name}` : P.name}</span>
              </SoftButton>
            </Row>
          </PanelSectionRow>
        )}
        <PanelSectionRow>
          <Block>
          <div style={{ ...card, background: "#1f252e", gap: 4 }}>
            <div style={{ display: "flex", alignItems: "center", gap: 8, fontWeight: 600, minWidth: 0 }}>
              <span style={{ width: 8, height: 8, borderRadius: 4, background: ready.length ? "#3ecf6e" : "#5c6470", flex: "none" }} />
              <span style={{ minWidth: 0 }}>
                {ready.length ? T?.name ?? PIDS[ready[0].pid] : "No controller connected"}
                {/* The battery icons right after the name. */}
                {ready.map((k) => (
                  <span key={k.addr} style={{ display: "inline-flex", marginLeft: 8, fontSize: 13, fontWeight: 400, verticalAlign: "middle" }}>
                    <Battery pct={k.battery_pct} label={ready.length > 1 ? sideLetter(k.pid) : undefined} />
                  </span>
                ))}
              </span>
            </div>
            <div style={hint}>{MODE_NAMES[runMode]}</div>
          </div>
          </Block>
        </PanelSectionRow>
        {pairing.required && (
          <PanelSectionRow>
            <ButtonItem layout="below" onClick={() => action("pair").then(poll)}>
              {pairing.open ? "Stop pairing" : "Pair a controller"}
            </ButtonItem>
          </PanelSectionRow>
        )}
        {upd?.available && !later && (
          <PanelSectionRow>
            <Banner
              text={`Firmware ${upd.latest} is available`}
              action="Update now"
              onLater={() => setLater(true)}
              onAction={() => showModal(<UpdateModal version={upd.latest!} onDone={() => checkUpdate(true).then(setUpd)} />)}
            />
          </PanelSectionRow>
        )}
        {pluginBanner}
      </PanelSection>

      {T && P && S && (
        <PanelSection title="Extra buttons">
          {extraButtons(t).map((e) => {
            const o = P.map[e.input];
            return (
              <PanelSectionRow key={e.input}>
                <Row
                  label={
                    <span style={{ display: "inline-flex", alignItems: "center", gap: 8 }}>
                      <InputIcon label={e.label} />
                      {e.side && <span style={hint}>{e.side}</span>}
                    </span>
                  }
                >
                  <SoftButton style={small} onActivate={() => pickOutput(e.input, e.label, e.side)}>
                    <OutputIcon mode={mode} idx={o} />
                  </SoftButton>
                </Row>
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
                  ? "Lay a Joy-Con on its side to use it as a mouse. Turning this on or off will restart the dongle."
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
                <ToggleField label="Horizontal scrolling" checked={!(flags & MF_UP_DOWN_ONLY)} onChange={(v) => setFlag(MF_UP_DOWN_ONLY, !v)} />
              </PanelSectionRow>
              <PanelSectionRow>
                <ToggleField label="Invert scroll up/down" checked={!!(flags & MF_INVERT_V)} onChange={(v) => setFlag(MF_INVERT_V, v)} />
              </PanelSectionRow>
              <PanelSectionRow>
                <ToggleField label="Invert scroll left/right" checked={!!(flags & MF_INVERT_H)} onChange={(v) => setFlag(MF_INVERT_H, v)} />
              </PanelSectionRow>
            </>
          )}
        </PanelSection>
      )}

      <PanelSection title="Dongle">
        {S && (
          <PanelSectionRow>
            <ToggleField
              label="Only on USB while a controller is connected"
              description={S.usb_detach ? "A controller must be connected to configure the dongle." : undefined}
              checked={!!S.usb_detach}
              onChange={(v) => save(`usb_detach=${v ? 1 : 0}`)}
            />
          </PanelSectionRow>
        )}
        {prefs && (
          <PanelSectionRow>
            <ToggleField
              label="Automatic update checks"
              description="On start and once a day"
              checked={prefs.check_updates}
              onChange={(v) => setPref("check_updates", v).then(setPrefs)}
            />
          </PanelSectionRow>
        )}
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            disabled={checking}
            onClick={async () => {
              // Now, for the dongle and this plugin; shows the banners again.
              setChecking(true);
              const [u, p] = await Promise.all([checkUpdate(true), checkPluginUpdate(true)]);
              setChecking(false);
              setUpd(u);
              setPupd(p);
              setLater(false);
              setPLater(false);
              if (!u.ok && !p.ok) toaster.toast({ title: "Switch2-Pico", body: u.error ?? p.error ?? "Couldn't check for updates" });
              else if (!u.available && !p.available) toaster.toast({ title: "Switch2-Pico", body: "Firmware and plugin are up to date" });
            }}
          >
            {checking ? "Checking…" : "Check for updates"}
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={() => showModal(<PairedModal onChange={poll} />)}>
            Paired controllers
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            onClick={() =>
              confirm("Reboot the dongle?", "The dongle will restart, and then you'll need to reconnect your controller.", "Reboot", () => {
                setRestarting(true);
                action("reboot");
              })
            }
          >
            Reboot dongle
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <Block>
          <div style={hint}>
            Firmware {st.version} ({st.platform})
            {upd?.ok && !upd.available && upd.latest ? " · up to date" : ""}
            {pupd?.installed ? ` · plugin ${pupd.installed}` : ""}
          </div>
          </Block>
        </PanelSectionRow>
      </PanelSection>
    </>
  );
}

// Decky's own installer (as for its store and "Install from URL"): it may
// replace the plugin's files and reloads the plugin; it asks to confirm.
// Older loaders without it: the backend does it (and restarts Decky).
async function updatePlugin(u: PluginUpdate) {
  const backend = (window as unknown as { DeckyBackend?: { call(route: string, ...args: unknown[]): Promise<unknown> } }).DeckyBackend;
  if (backend && u.url) {
    try {
      await backend.call("utilities/install_plugin", u.url, "Switch2-Pico", u.latest, u.sha256 ?? "", 2 /* update */);
      return;
    } catch (e) {
      console.warn("Switch2-Pico: Decky's installer failed, trying the plugin's own", e);
    }
  }
  confirm(`Update the plugin to ${u.latest}?`, "Decky will restart to load it.", "Update", async () => {
    const r = await installPluginUpdate();
    toaster.toast({
      title: "Switch2-Pico",
      body: r.ok ? `Plugin ${r.version} installed. Decky is restarting…` : `Plugin update failed: ${r.error}`,
    });
  });
}

// Toasts for the backend's "notify" events (also while the menu is closed).
async function onNotify(kind: string, a: string | number, b: string | number) {
  // Connects and low batteries: Steam shows those itself.
  if (kind === "update") {
    toaster.toast({ title: `Switch2-Pico ${a} available`, body: "Open the plugin to update" });
  } else if (kind === "plugin") {
    toaster.toast({ title: `Switch2-Pico plugin ${a} available`, body: "Open the plugin to update" });
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
