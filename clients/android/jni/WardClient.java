package uk.xa0.ward;

public final class WardClient implements AutoCloseable {
  static { System.loadLibrary("wardclient"); }

  private final long handle;

  public WardClient(String path) {
    handle = connect(path);
    if (handle == 0) throw new IllegalStateException("cannot connect to " + path);
  }

  public String createSession(String cwd) { return createSession(handle, cwd); }
  public boolean prompt(String session, String text) {
    return prompt(handle, session, text);
  }
  public byte[] call(String method, byte[] params) {
    return call(handle, method, params);
  }

  @Override public void close() { close(handle); }

  private static native long connect(String path);
  private static native String createSession(long handle, String cwd);
  private static native boolean prompt(long handle, String session, String text);
  private static native byte[] call(long handle, String method, byte[] params);
  private static native void close(long handle);

  public static void main(String[] args) {
    String path = args.length > 0 ? args[0] : System.getenv("WARD_SOCKET");
    try (WardClient client = new WardClient(path)) {
      String session = client.createSession("/tmp");
      System.out.println("session " + session);
    }
  }
}
