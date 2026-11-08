public final class HotWorkload {
    private static int mix(int value) {
        return Integer.rotateLeft(value * 0x45d9f3b + 0x27100001, 7) ^ 0x6d2b79f5;
    }

    public static void main(String[] args) {
        int checksum = 0;
        for (int i = 0; i < 500_000; i++) {
            checksum = mix(checksum ^ i);
        }
        System.out.println("iterations=500000 checksum=" + Integer.toUnsignedString(checksum));
    }
}
