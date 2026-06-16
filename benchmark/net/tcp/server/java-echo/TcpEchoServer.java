import java.io.IOException;
import java.net.StandardSocketOptions;
import java.nio.ByteBuffer;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;

public final class TcpEchoServer {
    private static final int DEFAULT_PORT = 9000;
    private static final int DEFAULT_WORKERS = 1;
    private static final int BACKLOG = 4096;
    private static final int BUFFER_SIZE = 65536;

    private TcpEchoServer() {
    }

    public static void main(String[] args) throws IOException {
        int port = parseIntArg(args, 0, DEFAULT_PORT);
        int workers = parseIntArg(args, 1, DEFAULT_WORKERS);

        if (System.getProperty("jdk.virtualThreadScheduler.parallelism") == null) {
            System.setProperty("jdk.virtualThreadScheduler.parallelism",
                               Integer.toString(workers));
        }
        if (System.getProperty("jdk.virtualThreadScheduler.maxPoolSize") == null) {
            System.setProperty("jdk.virtualThreadScheduler.maxPoolSize",
                               Integer.toString(workers));
        }

        ServerSocketChannel server = ServerSocketChannel.open();
        server.setOption(StandardSocketOptions.SO_REUSEADDR, true);
        server.bind(new java.net.InetSocketAddress("0.0.0.0", port), BACKLOG);

        System.err.printf(
            "java tcp echo server listening on 0.0.0.0:%d (virtual threads, carriers=%d)%n",
            port,
            workers);

        while (true) {
            SocketChannel socket = server.accept();
            socket.setOption(StandardSocketOptions.TCP_NODELAY, true);
            Thread.startVirtualThread(() -> handle(socket));
        }
    }

    private static int parseIntArg(String[] args, int index, int fallback) {
        if (args.length <= index) {
            return fallback;
        }
        try {
            int value = Integer.parseInt(args[index]);
            return value > 0 ? value : fallback;
        } catch (NumberFormatException ignored) {
            return fallback;
        }
    }

    private static void handle(SocketChannel socket) {
        try (socket) {
            ByteBuffer buffer = ByteBuffer.allocate(BUFFER_SIZE);
            while (true) {
                buffer.clear();
                int n = socket.read(buffer);
                if (n < 0) {
                    return;
                }
                buffer.flip();
                while (buffer.hasRemaining()) {
                    socket.write(buffer);
                }
            }
        } catch (IOException ignored) {
        }
    }
}
