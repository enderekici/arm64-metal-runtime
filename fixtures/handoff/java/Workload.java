import java.util.Arrays;
import java.util.List;
import java.util.stream.Collectors;

public final class Workload {
    public static void main(String[] args) {
        List<Integer> values = Arrays.stream(new int[] {7, 2, 9, 2, 5})
                .boxed()
                .sorted()
                .distinct()
                .collect(Collectors.toList());
        int checksum = values.stream().mapToInt(Integer::intValue).sum();
        System.out.println("values=" + values + " checksum=" + checksum);
    }
}
