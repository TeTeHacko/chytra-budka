import { useEffect, useState } from "preact/hooks";
import { api } from "./api";
import { DeviceGrid } from "./views/grid";
import { DeviceView } from "./views/device";
import { GalleryView } from "./views/gallery";
import { LoginView } from "./views/login";
import { auth, refreshAuth, toast, wsState } from "./store";

function useHashRoute(): string {
  const [route, setRoute] = useState(location.hash.slice(1) || "/");
  useEffect(() => {
    const on = () => setRoute(location.hash.slice(1) || "/");
    addEventListener("hashchange", on);
    return () => removeEventListener("hashchange", on);
  }, []);
  return route;
}

export function App() {
  const route = useHashRoute();
  const a = auth.value;

  if (a === null) return <p class="muted center">…</p>;
  if (a.mode !== "off" && !a.logged_in) return <LoginView />;

  let view;
  const devMatch = route.match(/^\/device\/(cb-[a-f0-9]{6})$/);
  if (devMatch) view = <DeviceView id={devMatch[1]} />;
  else if (route === "/gallery") view = <GalleryView />;
  else view = <DeviceGrid />;

  const logout = async () => {
    await api.post("/api/logout");
    await refreshAuth();
  };

  return (
    <>
      <header>
        <a class="brand" href="#/">🐦 Chytrá budka</a>
        <nav>
          <a href="#/" class={route === "/" ? "active" : ""}>Zařízení</a>
          <a href="#/gallery" class={route === "/gallery" ? "active" : ""}>Galerie</a>
        </nav>
        {a.user && a.user !== "anonymous" && (
          <button class="link" title={a.user} onClick={logout}>odhlásit</button>
        )}
        <span class={`ws ws-${wsState.value}`} title={`live: ${wsState.value}`}>●</span>
      </header>
      <main>{view}</main>
      {toast.value && <div class="toast">{toast.value}</div>}
    </>
  );
}
