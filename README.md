# RT-Predict
A machine leraning infrastructure to predict the usage pattern of a black-box application leveragin linux perf conters.

This project was developed as the final project for the Boston University CS506 class, taken in Spring 2022.

The complete proposal can be found [here](https://docs.google.com/document/d/1hzWPh93EJD95I3LjsqBPXn79j3DJFD2RTzB84ytnrGs/edit?usp=sharing).

The project report can be forund [here](https://docs.google.com/document/d/1cEs_xwEqi64cJu7z-chhZsXmHAnSQ3dqovTtNbg2d0I/edit?usp=sharing).

## Features
- benchmark: the benchmark executable name
- input: the input size format
- pid: the process id
- function: the function name
- call occurrence: represents the ith call of a certain function in a process
- runtime: the function / app runtime in seconds
- cache misses: the number of cache misses during the function / application
- cache references: the number of cache references during the function application
- retired instructions: the number of instructions executed during the function / application
- branch misses: the number of branch mises during the function / application
- page faults: the number of page faults during the function / application
