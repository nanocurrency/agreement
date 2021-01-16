import csv
import glob
import matplotlib.pyplot as plt
import os
import sys

if (len (sys.argv) != 2):
	print ("Usage: graph <directory/file>")
	exit (1)

files = []
if os.path.isdir (sys.argv[1]):
	files = glob.glob (sys.argv[1] + "/*.csv")
else:
	files = [sys.argv[1]]
print (files)


for file in files:
	print (file)
	labels = []
	value_samples = {}

	with open (file, 'r') as file:
		points = csv.reader (file)
		samples = -1
		last_time = None
		for row in points:
			time, value, weight = row
			if time != last_time:
				last_time = time
				samples = samples + 1
				labels.append (time)
			if value not in value_samples.keys ():
				print (value)
				new_samples = []
				for i in range (samples):
					new_samples.append (0)
				value_samples[value] = new_samples
			value_samples[value].append (int(weight))
			print (labels, len (labels))
			print (value_samples)
			for value in value_samples.values():
				print (len(value))

	fig, ax = plt.subplots()

	bottom = None
	width = 0.35

	ax.stackplot(labels, value_samples.values(), labels=value_samples.keys())

	ax.set_ylabel ('Weight')
	ax.set_title ('Value weight at time')
	ax.legend ()
	plt.show ()
