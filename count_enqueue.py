import sys

nums = {}
total = 0;
if __name__ == "__main__":
	filename = sys.argv[1];
	with open(filename) as file:
		for line in file.readlines():
			if (line.startswith("grep_bin")
				or line.startswith("N")
				or line.startswith("rootpath")
				or line.startswith("searchstr")
				or len(line.strip()) == 0
				):
					continue

			if line.endswith("] DONE\n"):
				continue

			print(line)
			tid, action, path = line.split(" ", 2);
			if tid not in nums:
				nums[tid] = 0

			if action == "ENQUEUE":
				nums[tid] += 1
				total +=1

	for tid, n in nums.items():
		print(tid, n)

	print("total:", total)
	print("average:", total / len(nums))
