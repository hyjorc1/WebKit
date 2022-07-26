import argparse
import collections
import json
import math
import os
import re
import subprocess
import sys
import pandas as pd
import numpy as np
from scipy.stats import ttest_ind

totalScoreRE = re.compile(r"Total Score:  (\d+(?:.\d+)?)")

testDir = "/Volumes/WebKit/mac/OpenSource/PerformanceTests/JetStream2"
vmPath = "/Volumes/WebKit/mac/OpenSource/WebKitBuild/Release"
jscPath = vmPath + "/jsc"

def runMinimodeTest(epoch, extraOptions=[]):
    options = [jscPath, "--useJIT=0"] + extraOptions + ["watch-cli.js"]
    proc = subprocess.Popen(options, cwd=testDir, env={"DYLD_FRAMEWORK_PATH":vmPath}, stdout=subprocess.PIPE, stdin=subprocess.PIPE, stderr=None, shell=False)
    totalScore = 0
    while True:
        line = proc.stdout.readline()
        if sys.version_info[0] >= 3:
            line = str(line, "utf-8")
        # print(line)
        totalScoreSearch = re.search(totalScoreRE, str(line.strip()))
        if totalScoreSearch:
            totalScore = float(totalScoreSearch.group(1))
        if line == "":
            break
    print("score {}: {} with options: {}".format(epoch, totalScore, options))
    return totalScore

def getScores(epoch, extraOptions=[]):
    scores = []
    for i in range(epoch):
        scores.append(runMinimodeTest(i, extraOptions))
    return scores

def returnComparedResults(baseScores, configScores, extraOptions):
    baseStat = pd.Series(baseScores).describe()
    configStat = pd.Series(configScores).describe()
    pvalue = ttest_ind(baseScores, configScores, equal_var=False).pvalue
    print("pvalue: ", pvalue)
    sig = pvalue <= 0.05
    prog = (configStat["mean"] - baseStat["mean"]) * 1.0 / baseStat["mean"] * 100
    print("base mean: {:.3f}, config mean: {:.3f}, prog: {:.2f}%, sig: {}, options: {}".format(baseStat["mean"], configStat["mean"], prog, sig, extraOptions))

def aabb(epoch, extraOptions=[]):
    baseScores = getScores(epoch)
    configScores = getScores(epoch, extraOptions)
    returnComparedResults(baseScores, configScores, epoch)

def abab(epoch, extraOptions=[]):
    baseScores = []
    configScores = []
    for i in range(epoch):
        baseScores += getScores(1)
        configScores += getScores(1, extraOptions)
    return returnComparedResults(baseScores, configScores, extraOptions)

abab(10, [
    "--validateOptions=1",
    "--maximumFunctionForCallInlineCandidateBytecodeCost=100",
    "--maximumFunctionForClosureCallInlineCandidateBytecodeCost=80"
    "--maximumFunctionForConstructInlineCandidateBytecoodeCost=80"
])
