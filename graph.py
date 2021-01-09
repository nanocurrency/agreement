import matplotlib.pyplot as plt
import csv

labels = []
value_samples = {}

with open ('edges.csv', 'r') as file:
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
            new_samples = []
            for i in range (samples):
                new_samples.append (0)
            value_samples[value] = new_samples
        value_samples[value].append (int(weight))

fig, ax = plt.subplots()

bottom = None
width = 0.35
for val in value_samples:
    items = value_samples[val]
    ax.bar (labels, items, width, bottom=bottom, label=val)
    bottom = items

ax.set_ylabel ('Weight')
ax.set_title ('Value weight at time')
ax.legend ()
plt.show ()
