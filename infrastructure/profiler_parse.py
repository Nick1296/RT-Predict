import os
import pickle

import numpy as np
from tqdm.auto import tqdm
import matplotlib.pyplot as plt

# column numbers
bmark = 0
input_size = 1
pid = 2
fname = 3

# timeframes
sample_period = 0.0001

data_path = "data/"
counters = 5


def read_datafile(file_name):
    data = None
    occ = {}
    print("loading raw data")
    raw_data = np.genfromtxt(file_name,
                             delimiter=",",
                             skip_header=0,
                             dtype="unicode")
    print("loaded raw data")
    columns = raw_data[0]
    raw_data = raw_data[1:]
    columns = np.insert(columns, 4, "call occurrence")
    columns = np.insert(columns, 5, "runtime (sec)")  # print(raw_data[0])
    # the profiler will make sure to zero the initial counter values
    prev_row = np.zeros(len(raw_data[0]), dtype="int64")
    stack = []
    prev_id = ""
    elapsed_time = 0
    print("processing raw data")
    for i in tqdm(range(raw_data.shape[0])):
        elapsed_time = round(elapsed_time + sample_period, 4)
        row = raw_data[i]
        str_id = str(row[pid]) + str(row[fname])

        if prev_row[pid] != row[pid]:
            # the counters have been reset by the profiler when
            # a new executable is being profiled
            # so this row is the first row for the current process
            prev_row = np.zeros(len(raw_data[0]), dtype="int64")
        # calculating the delta between the counters
        cntr = row[fname + 1:].astype("int64") - prev_row[fname +
                                                          1:].astype("int64")
        # for j in range(fname + 1, fname + 1 + counters):
        #    cntr.append(float(row[j]) - float(prev_row[j]))

        # calculating the number of occurrences of a function.
        if str_id != prev_id and not (str_id in stack):
            if str_id not in occ:
                occ[str_id] = 0
            stack.append(str_id)
        else:
            while stack[-1] != str_id:
                occ[stack[-1]] = occ[stack[-1]] + 1
                stack.pop()
        # print(stack)
        if np.any(cntr > 0) and not np.any(cntr < 0):
            new_row = np.array([
                row[bmark],
                row[input_size],
                row[pid],
                row[fname],
                occ[str_id],
                elapsed_time,
            ])
            new_row = np.append(new_row, cntr)
            if data is None:
                # data = new_row
                data = [new_row]
            else:
                # data = np.vstack([data, new_row])
                data.append(new_row)
            elapsed_time = 0

        prev_id = str_id
        prev_row = row

    while len(stack) != 0:
        occ[stack[-1]] = occ[stack[-1]] + 1
        stack.pop()
    # print(occ)
    # print(columns)
    # for i in range(0, len(data)):
    #    print(data[i])
    data = np.array(data)
    outfile = open(os.path.join(data_path, "cleaned_data_no_header.pkl"), "wb")
    pickle.dump(data, outfile)
    outfile.close()
    return data, columns


def plot_execution(fn):
    pkl_fn = data_path + "cleaned_data_no_header.pkl"
    if os.path.exists(pkl_fn):
        data_fd = open(pkl_fn, mode='rb')
        data = pickle.load(data_fd)
    else:
        data, cols = read_datafile(data_path + "single_run_test.csv")

    labels=["cache misses", "cache references"]
    #For each counter have a different plot
    for cnt in range(2):
        x = []
        y = []
        func_line = []
        func_nm = []
        x.append(0)
        y.append(0)
        prev_func_nm = ""
        # loop through the data to build the x and y for the cummulative graphs.
        for row in data[0:1000]:
            elapsed_time = x[-1] + row[5]
            y_val = y[-1] + row[cnt + 6]

            if(row[3] != prev_func_nm):
                func_line.append(elapsed_time)
                func_nm.append(row[3])
                prev_func_nm = row[3]

            for j in np.arange(0, row[5], sample_period):
                x.append(x[-1] + sample_period)
                y.append(y[-1])
            y[-1] = y_val
        plt.plot(x, y,  marker='o', c=colors[cnt], ms=1, label=labels[cnt])
        plt.xlim([0.91, 0.92])
        plt.ylim([0, 12000000])
        plt.ylabel("Cummul. Counter Value")
        plt.xlabel("Time (s)")



        # print(func_line, x[0:10], y[0:10])
        for i in range(len(func_line)):
            plt.axvline(func_line[i], lw=1)
            plt.text(func_line[i], 1000000, func_nm[i], rotation=90, fontsize=7)

        plt.legend(loc=1)

    plt.show()

def main():
    # read_datafile(data_path + "single_run_test.csv")
    # read_datafile(data_path + "test.csv")
    read_datafile(data_path + "10_runs_5_inputs_5_bmarks_raw.csv")


if __name__ == "__main__":
    main()
