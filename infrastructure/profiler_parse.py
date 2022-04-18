import os
import pickle

import numpy as np
from tqdm.auto import tqdm

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


def main():
    # read_datafile(data_path + "single_run_test.csv")
    # read_datafile(data_path + "test.csv")
    read_datafile(data_path + "10_runs_5_inputs_5_bmarks_raw.csv")


if __name__ == "__main__":
    main()
