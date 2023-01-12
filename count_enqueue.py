import sys

nums = {}
total = 0;
paths = set()
dupes = {}
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

			# print(line)
			try:
				tid, action, path = line.split(" ", 2);
			except:
				continue
			if ("/" not in path):
				continue

			if action == "ENQUEUE":
				if tid not in nums:
					nums[tid] = 0

				print(action, path.strip(), tid)#, "(", line.strip(), ")")
				nums[tid] += 1
				total +=1
				if path in paths:
					if path not in dupes:
						dupes[path] = []
					dupes[path].append(tid)
				paths.add(path)

	for tid, n in nums.items():
		print(tid, n)

	print("total:", total)
	print("average:", total / len(nums))
	print("dupes:", "\n".join(dupes))
