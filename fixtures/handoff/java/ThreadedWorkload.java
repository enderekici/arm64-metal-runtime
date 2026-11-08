public final class ThreadedWorkload {
    private static final Object GATE = new Object();
    private static boolean workerWaiting;
    private static boolean releaseWorker;
    private static volatile boolean workerFinished;
    private static volatile long workerChecksum;

    private static long hotLoop(long seed) {
        long checksum = seed;
        for (int i = 0; i < 500_000; i++) {
            checksum = Long.rotateLeft(checksum ^ (i * 0x9e3779b9L), 9)
                    + 0x632be59bd9b4e019L;
        }
        return checksum;
    }

    private static void worker() {
        synchronized (GATE) {
            workerWaiting = true;
            GATE.notifyAll();
            while (!releaseWorker) {
                try {
                    GATE.wait();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError("worker interrupted", interrupted);
                }
            }
        }
        workerChecksum = hotLoop(0x13579bdf2468ace0L);
        workerFinished = true;
    }

    public static void main(String[] args) throws InterruptedException {
        Thread thread = new Thread(ThreadedWorkload::worker, "threaded-fixture-worker");
        thread.start();
        synchronized (GATE) {
            while (!workerWaiting) {
                GATE.wait();
            }
            releaseWorker = true;
            GATE.notifyAll();
        }
        thread.join();
        if (!workerFinished || thread.isAlive()) {
            throw new AssertionError("worker did not finish before join returned");
        }
        System.err.printf("THREAD_OK started=true wait_notify=true joined=true finished=true "
                + "iterations=500000 checksum=%d%n", workerChecksum);
    }
}
