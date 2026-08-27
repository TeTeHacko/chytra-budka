import { useState } from "preact/hooks";
import { api, ApiError } from "../api";
import { auth, connectWs, refreshAuth, refreshDevices } from "../store";

export function LoginView() {
  const mode = auth.value?.mode ?? "password";
  const [password, setPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (e: Event) => {
    e.preventDefault();
    setBusy(true);
    setError(null);
    try {
      await api.post("/api/login", { password });
      await refreshAuth();
      await refreshDevices();
      connectWs();
    } catch (err) {
      setError(err instanceof ApiError && err.status === 401
        ? "Špatné heslo" : "Přihlášení selhalo");
    } finally {
      setBusy(false);
    }
  };

  return (
    <div class="login">
      <h2>🐦 Chytrá budka</h2>
      {mode === "oidc" ? (
        <a class="button" href="/auth/oidc/login">Přihlásit přes SSO</a>
      ) : (
        <form onSubmit={submit}>
          <input
            type="password"
            placeholder="heslo operátora"
            value={password}
            onInput={(e: Event) => setPassword((e.target as HTMLInputElement).value)}
            autofocus
          />
          <button type="submit" disabled={busy || !password}>Přihlásit</button>
        </form>
      )}
      {error && <p class="error">{error}</p>}
    </div>
  );
}
