"""husk check of Karma's sky dome against Karma (docs/design/M11-P3.md).

The scenes in karma_sky/ were built by Houdini's own LOPs (karma_sky/
make_scenes.py, isolated hython): the Karma Sky Dome Light at several solar
altitudes, azimuths, turbidities, ground albedos and colours, horizon blurs,
light colours and a rotated light, and the Karma Physical Sky (sky mode: a
sun DistantLight plus the sky dome) at solar altitudes 5/30/60 degrees and
turbidities 2/3/6. Each has two cameras with pixel-centre sampling (box
filter of zero width), so a sky pixel is the radiance in its center
direction:
  * /Render/up: towards the zenith, 170 degree field of view;
  * /Render/front: along -Z, 120 degrees (horizon, horizon fade, the ground
    colour below it), which also sees a white Lambert quad facing up below
    the camera, whose radiance is the sky's (and sun's) irradiance over pi.
Karma CPU (Houdini 22.0.466) was measured once with husk at 1024 samples per
pixel; its values are stored below. Nova renders the same files through
husk (CPU, and Nova XPU for the Physical Sky scenes) and must match:
  * sky and ground pixels within 2 % (Nova bakes the sky into a latlong of
    0.18 degree texels; measured within 0.01 %);
  * pixels inside the horizon fade within 3 % + 0.01 (the bake interpolates
    the fade, about 1 degree wide by default, linearly between rows);
  * the quad's mean (irradiance) within 2 % (measured 0.0..0.8 %: Karma's
    default MIS on the sky is biased high by up to 0.8 %, Nova matches the
    model's integral within 0.01 %);
  * a sky whose light link excludes the quad leaves it black (Karma too).

Needs numpy and OpenImageIO, so run it with Houdini's Python:
$HFS/python313/python.exe -I check_karma_sky.py --hfs ... --stage ... --out ...
"""

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_scenes import FAILURES, check, isolated_env, run, tool  # noqa: E402  (shared husk helpers)

SCENES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "karma_sky")
PATCH = (slice(128, 157), slice(72, 89))  # rows and columns inside the white quad (front view)
TOL = {"sky": (0.02, 1e-4), "ground": (0.02, 1e-4), "fade": (0.03, 0.01)}  # relative, absolute

# scene -> (Karma's patch mean RGB, [(view, row, column, kind, Karma's RGB)]).
# Pixels within 3 degrees of the Physical Sky's sun disc are left out.
KARMA = {
    "ps_a30_t2": ((0.234656, 0.207738, 0.195803), [
        ("up", 80, 80, "sky", (0.0157042, 0.0286924, 0.0594448)),
        ("up", 30, 80, "sky", (0.0949942, 0.150494, 0.203074)),
        ("up", 130, 80, "sky", (0.136102, 0.192937, 0.240627)),
        ("up", 80, 25, "sky", (0.0843045, 0.133011, 0.181953)),
        ("up", 80, 135, "sky", (0.0987395, 0.147673, 0.19302)),
        ("up", 20, 20, "sky", (0.132582, 0.193786, 0.232325)),
        ("up", 140, 140, "sky", (0.19918, 0.257607, 0.283266)),
        ("front", 20, 80, "sky", (0.0306108, 0.0502704, 0.0892494)),
        ("front", 50, 30, "sky", (0.0389629, 0.0707193, 0.119057)),
        ("front", 65, 130, "sky", (0.134037, 0.188899, 0.242463)),
        ("front", 75, 60, "sky", (0.12156, 0.171581, 0.207076)),
        ("front", 80, 40, "fade", (0.0466463, 0.041253, 0.0389571)),
        ("front", 95, 20, "ground", (0.0464831, 0.0410003, 0.0387055)),
        ("front", 110, 150, "ground", (0.0464831, 0.0410003, 0.0387055))]),
    "ps_a30_t3": ((0.237987, 0.210614, 0.197015), [
        ("up", 80, 80, "sky", (0.0180693, 0.0309189, 0.060848)),
        ("up", 30, 80, "sky", (0.0942496, 0.145786, 0.194855)),
        ("up", 130, 80, "sky", (0.178137, 0.225318, 0.255969)),
        ("up", 80, 25, "sky", (0.0854436, 0.130074, 0.176159)),
        ("up", 80, 135, "sky", (0.108447, 0.151209, 0.191344)),
        ("up", 20, 20, "sky", (0.130379, 0.185156, 0.220246)),
        ("up", 140, 140, "sky", (0.289792, 0.325749, 0.316712)),
        ("front", 20, 80, "sky", (0.0413165, 0.0603692, 0.0962168)),
        ("front", 50, 30, "sky", (0.0435138, 0.0733796, 0.11933)),
        ("front", 65, 130, "sky", (0.204829, 0.250008, 0.277785)),
        ("front", 75, 60, "sky", (0.136176, 0.17737, 0.205961)),
        ("front", 80, 40, "fade", (0.0472925, 0.0418312, 0.0392162)),
        ("front", 95, 20, "ground", (0.0471304, 0.0415967, 0.0389913)),
        ("front", 110, 150, "ground", (0.0471304, 0.0415967, 0.0389913))]),
    "ps_a30_t6": ((0.250723, 0.220963, 0.200024), [
        ("up", 80, 80, "sky", (0.0304748, 0.0431787, 0.0677776)),
        ("up", 30, 80, "sky", (0.0950102, 0.125049, 0.164378)),
        ("up", 130, 80, "sky", (0.33589, 0.336185, 0.305547)),
        ("up", 80, 25, "sky", (0.0900343, 0.116033, 0.150942)),
        ("up", 80, 135, "sky", (0.143118, 0.163016, 0.184551)),
        ("up", 20, 20, "sky", (0.120467, 0.146786, 0.175648)),
        ("up", 140, 140, "sky", (0.593245, 0.544483, 0.417539)),
        ("front", 20, 80, "sky", (0.0931701, 0.105319, 0.123427)),
        ("front", 50, 30, "sky", (0.059451, 0.0829378, 0.120076)),
        ("front", 65, 130, "sky", (0.479827, 0.466603, 0.395842)),
        ("front", 75, 60, "sky", (0.18339, 0.195148, 0.200405)),
        ("front", 80, 40, "fade", (0.0498468, 0.0439759, 0.0399146)),
        ("front", 95, 20, "ground", (0.0496711, 0.0437899, 0.0397568)),
        ("front", 110, 150, "ground", (0.0496711, 0.0437899, 0.0397568))]),
    "ps_a5_t2": ((0.0385872, 0.0354691, 0.0335415), [
        ("up", 80, 80, "sky", (0.00811402, 0.0117382, 0.0184485)),
        ("up", 30, 80, "sky", (0.0727289, 0.0775825, 0.0586818)),
        ("up", 130, 80, "sky", (0.102986, 0.0990658, 0.0704413)),
        ("up", 80, 25, "sky", (0.0597575, 0.0646899, 0.0492391)),
        ("up", 80, 135, "sky", (0.0661338, 0.0702092, 0.0534033)),
        ("up", 20, 20, "sky", (0.101274, 0.0950327, 0.0580443)),
        ("up", 140, 140, "sky", (0.202055, 0.155367, 0.0783051)),
        ("front", 20, 80, "sky", (0.013589, 0.0197667, 0.0272652)),
        ("front", 50, 30, "sky", (0.0234284, 0.0353816, 0.0413357)),
        ("front", 65, 130, "sky", (0.105724, 0.100164, 0.0754762)),
        ("front", 75, 60, "sky", (0.0833615, 0.0804333, 0.0532231)),
        ("front", 80, 40, "fade", (0.00744703, 0.00692357, 0.00670893)),
        ("front", 95, 20, "ground", (0.00729226, 0.00680659, 0.00666891)),
        ("front", 110, 150, "ground", (0.00729226, 0.00680659, 0.00666891))]),
    "ps_a5_t3": ((0.0386475, 0.0358899, 0.033506), [
        ("up", 80, 80, "sky", (0.00803645, 0.0120858, 0.0187504)),
        ("up", 30, 80, "sky", (0.0691267, 0.0738136, 0.0567837)),
        ("up", 130, 80, "sky", (0.135929, 0.113788, 0.0712457)),
        ("up", 80, 25, "sky", (0.0581169, 0.0622559, 0.0479375)),
        ("up", 80, 135, "sky", (0.0696084, 0.0699764, 0.0522516)),
        ("up", 20, 20, "sky", (0.093278, 0.0877769, 0.0555483)),
        ("up", 140, 140, "sky", (0.326983, 0.215138, 0.0852618)),
        ("front", 20, 80, "sky", (0.0147872, 0.0207712, 0.0274367)),
        ("front", 50, 30, "sky", (0.0256921, 0.0368229, 0.0411632)),
        ("front", 65, 130, "sky", (0.163377, 0.129339, 0.0798855)),
        ("front", 75, 60, "sky", (0.0886547, 0.0801727, 0.0518574)),
        ("front", 80, 40, "fade", (0.0073326, 0.00694823, 0.00669218)),
        ("front", 95, 20, "ground", (0.00719373, 0.00685284, 0.00665972)),
        ("front", 110, 150, "ground", (0.00719373, 0.00685284, 0.00665972))]),
    "ps_a5_t6": ((0.0395413, 0.0369676, 0.0334877), [
        ("up", 80, 80, "sky", (0.00925728, 0.0135118, 0.0195641)),
        ("up", 30, 80, "sky", (0.0586481, 0.0601472, 0.0476759)),
        ("up", 130, 80, "sky", (0.206215, 0.140097, 0.068272)),
        ("up", 80, 25, "sky", (0.0495902, 0.0505667, 0.039631)),
        ("up", 80, 135, "sky", (0.0751886, 0.0645137, 0.0450933)),
        ("up", 20, 20, "sky", (0.0660367, 0.0617384, 0.0438944)),
        ("up", 140, 140, "sky", (0.542284, 0.302967, 0.0879476)),
        ("front", 20, 80, "sky", (0.0207734, 0.0248768, 0.0289025)),
        ("front", 50, 30, "sky", (0.0336587, 0.0405781, 0.0406243)),
        ("front", 65, 130, "sky", (0.307205, 0.197444, 0.0852121)),
        ("front", 75, 60, "sky", (0.0925053, 0.0714728, 0.0438303)),
        ("front", 80, 40, "fade", (0.00733648, 0.00713653, 0.00671995)),
        ("front", 95, 20, "ground", (0.00725205, 0.0070863, 0.00670413)),
        ("front", 110, 150, "ground", (0.00725205, 0.0070863, 0.00670413))]),
    "ps_a60_t2": ((0.408732, 0.362073, 0.349959), [
        ("up", 80, 80, "sky", (0.0284906, 0.0468999, 0.098027)),
        ("up", 30, 80, "sky", (0.0864008, 0.145148, 0.227937)),
        ("up", 130, 80, "sky", (0.104988, 0.167597, 0.253905)),
        ("up", 80, 25, "sky", (0.0884511, 0.145447, 0.224859)),
        ("up", 80, 135, "sky", (0.0985854, 0.157174, 0.237285)),
        ("up", 20, 20, "sky", (0.11276, 0.17691, 0.256329)),
        ("up", 140, 140, "sky", (0.131954, 0.197888, 0.27857)),
        ("front", 20, 80, "sky", (0.0478188, 0.0751084, 0.136354)),
        ("front", 50, 30, "sky", (0.0434347, 0.0820565, 0.151955)),
        ("front", 65, 130, "sky", (0.0874412, 0.146092, 0.234366)),
        ("front", 75, 60, "sky", (0.115252, 0.175536, 0.251465)),
        ("front", 80, 40, "fade", (0.0814783, 0.0721331, 0.0697407)),
        ("front", 95, 20, "ground", (0.0813496, 0.0719001, 0.069468)),
        ("front", 110, 150, "ground", (0.0813496, 0.0719001, 0.069468))]),
    "ps_a60_t3": ((0.41185, 0.367039, 0.353553), [
        ("up", 80, 80, "sky", (0.0425962, 0.0622593, 0.109604)),
        ("up", 30, 80, "sky", (0.088636, 0.143538, 0.223599)),
        ("up", 130, 80, "sky", (0.115395, 0.173145, 0.256373)),
        ("up", 80, 25, "sky", (0.0917766, 0.14419, 0.219857)),
        ("up", 80, 135, "sky", (0.10468, 0.158604, 0.235861)),
        ("up", 20, 20, "sky", (0.114748, 0.172448, 0.248598)),
        ("up", 140, 140, "sky", (0.145574, 0.203175, 0.278588)),
        ("front", 20, 80, "sky", (0.0795743, 0.108611, 0.161821)),
        ("front", 50, 30, "sky", (0.0468625, 0.0858475, 0.154708)),
        ("front", 65, 130, "sky", (0.0996634, 0.155215, 0.240335)),
        ("front", 75, 60, "sky", (0.122716, 0.17635, 0.248415)),
        ("front", 80, 40, "fade", (0.0821093, 0.0731493, 0.0705034)),
        ("front", 95, 20, "ground", (0.0819763, 0.0729352, 0.070253)),
        ("front", 110, 150, "ground", (0.0819763, 0.0729352, 0.070253))]),
    "ps_a60_t6": ((0.43029, 0.388935, 0.37321), [
        ("up", 80, 80, "sky", (0.110282, 0.127846, 0.165143)),
        ("up", 30, 80, "sky", (0.0913742, 0.139407, 0.211459)),
        ("up", 130, 80, "sky", (0.16066, 0.206913, 0.271025)),
        ("up", 80, 25, "sky", (0.0977615, 0.142452, 0.209034)),
        ("up", 80, 135, "sky", (0.128271, 0.172476, 0.237486)),
        ("up", 20, 20, "sky", (0.111286, 0.156598, 0.220595)),
        ("up", 140, 140, "sky", (0.201029, 0.239292, 0.281844)),
        ("front", 20, 80, "sky", (0.227167, 0.251405, 0.275245)),
        ("front", 50, 30, "sky", (0.0649327, 0.104651, 0.171021)),
        ("front", 65, 130, "sky", (0.154029, 0.20219, 0.270781)),
        ("front", 75, 60, "sky", (0.149785, 0.189418, 0.243533)),
        ("front", 80, 40, "fade", (0.0858816, 0.0776691, 0.0746381)),
        ("front", 95, 20, "ground", (0.0857776, 0.0775149, 0.0744262)),
        ("front", 110, 150, "ground", (0.0857776, 0.0775149, 0.0744262))]),
    "sd_a15_t10_alb0": ((0.0864563, 0.0802749, 0.0741287), [
        ("up", 80, 80, "sky", (0.0246168, 0.0310213, 0.0394097)),
        ("up", 30, 80, "sky", (0.0458093, 0.0474429, 0.0549416)),
        ("up", 130, 80, "sky", (1.17592, 0.817847, 0.472114)),
        ("up", 80, 25, "sky", (0.0483044, 0.0443008, 0.0462721)),
        ("up", 80, 135, "sky", (0.0481081, 0.0440221, 0.0458858)),
        ("up", 20, 20, "sky", (0.0357672, 0.0345482, 0.038487)),
        ("up", 140, 140, "sky", (0.17238, 0.132469, 0.0919965)),
        ("front", 20, 80, "sky", (0.11151, 0.102454, 0.0867817)),
        ("front", 50, 30, "sky", (0.152402, 0.13392, 0.110899)),
        ("front", 65, 130, "sky", (0.169801, 0.138824, 0.107429)),
        ("front", 75, 60, "sky", (0.495932, 0.356108, 0.209532)),
        ("front", 80, 40, "fade", (0.000446447, 0.00032149, 0.000187813)),
        ("front", 95, 20, "ground", (0, 0, 0)),
        ("front", 110, 150, "ground", (0, 0, 0))]),
    "sd_a30_t6_az90": ((0.0898444, 0.107431, 0.132017), [
        ("up", 80, 80, "sky", (0.0314316, 0.0464991, 0.0747535)),
        ("up", 30, 80, "sky", (0.111652, 0.141561, 0.177746)),
        ("up", 130, 80, "sky", (0.112578, 0.142322, 0.178102)),
        ("up", 80, 25, "sky", (0.128638, 0.163705, 0.205452)),
        ("up", 80, 135, "sky", (0.7461, 0.697372, 0.544049)),
        ("up", 20, 20, "sky", (0.13634, 0.163642, 0.192057)),
        ("up", 140, 140, "sky", (0.25868, 0.268271, 0.25766)),
        ("front", 20, 80, "sky", (0.0356102, 0.0543455, 0.0869116)),
        ("front", 50, 30, "sky", (0.0557104, 0.0845925, 0.128766)),
        ("front", 65, 130, "sky", (0.202039, 0.226376, 0.244003)),
        ("front", 75, 60, "sky", (0.125449, 0.151859, 0.180231)),
        ("front", 80, 40, "fade", (0.000326129, 0.000344115, 0.000324654)),
        ("front", 95, 20, "ground", (0, 0, 0)),
        ("front", 110, 150, "ground", (0, 0, 0))]),
    "sd_a5_t2": ((0.0216703, 0.0289021, 0.0337037), [
        ("up", 80, 80, "sky", (0.00833989, 0.0123, 0.0192319)),
        ("up", 30, 80, "sky", (0.0834364, 0.0881312, 0.065576)),
        ("up", 80, 25, "sky", (0.0550782, 0.0614639, 0.0487425)),
        ("up", 80, 135, "sky", (0.055569, 0.0617124, 0.0486065)),
        ("up", 20, 20, "sky", (0.0854026, 0.0826329, 0.0527694)),
        ("up", 140, 140, "sky", (0.100351, 0.0936627, 0.0595903)),
        ("front", 20, 80, "sky", (0.0151135, 0.0214325, 0.0289937)),
        ("front", 50, 30, "sky", (0.0321827, 0.0440513, 0.0487911)),
        ("front", 65, 130, "sky", (0.0582337, 0.0681291, 0.0610377)),
        ("front", 75, 60, "sky", (0.154332, 0.13073, 0.0758851)),
        ("front", 80, 40, "fade", (0.000284592, 0.000218881, 9.87688e-05)),
        ("front", 95, 20, "ground", (0, 0, 0)),
        ("front", 110, 150, "ground", (0, 0, 0))]),
    "sd_a60_t3_az200": ((0.0575388, 0.0953241, 0.16308), [
        ("up", 80, 80, "sky", (0.0323862, 0.0560232, 0.110952)),
        ("up", 30, 80, "sky", (0.150291, 0.223495, 0.322686)),
        ("up", 130, 80, "sky", (0.120574, 0.191689, 0.288489)),
        ("up", 80, 25, "sky", (0.131633, 0.199004, 0.289621)),
        ("up", 80, 135, "sky", (0.124814, 0.191483, 0.281036)),
        ("up", 20, 20, "sky", (0.180432, 0.252491, 0.341117)),
        ("up", 140, 140, "sky", (0.152628, 0.225224, 0.314592)),
        ("front", 20, 80, "sky", (0.0266399, 0.0554575, 0.114688)),
        ("front", 50, 30, "sky", (0.0538029, 0.100592, 0.179967)),
        ("front", 65, 130, "sky", (0.0916705, 0.155214, 0.249006)),
        ("front", 75, 60, "sky", (0.145919, 0.216159, 0.304227)),
        ("front", 80, 40, "fade", (0.000431631, 0.000525054, 0.000573181)),
        ("front", 95, 20, "ground", (0, 0, 0)),
        ("front", 110, 150, "ground", (0, 0, 0))]),
    "sd_a85_t1_alb1": ((0.0639382, 0.116428, 0.208956), [
        ("up", 80, 80, "sky", (0.0515144, 0.0901657, 0.155057)),
        ("up", 30, 80, "sky", (0.174745, 0.289908, 0.425325)),
        ("up", 130, 80, "sky", (0.178296, 0.296124, 0.43482)),
        ("up", 80, 25, "sky", (0.184317, 0.302928, 0.439148)),
        ("up", 80, 135, "sky", (0.185969, 0.30506, 0.441181)),
        ("up", 20, 20, "sky", (0.220173, 0.346254, 0.474517)),
        ("up", 140, 140, "sky", (0.222659, 0.350034, 0.479154)),
        ("front", 20, 80, "sky", (0.046493, 0.0888945, 0.175035)),
        ("front", 50, 30, "sky", (0.0843805, 0.156219, 0.268526)),
        ("front", 65, 130, "sky", (0.138936, 0.241289, 0.37477)),
        ("front", 75, 60, "sky", (0.215891, 0.342668, 0.474975)),
        ("front", 80, 40, "fade", (0.000640004, 0.000850254, 0.000963163)),
        ("front", 95, 20, "ground", (0, 0, 0)),
        ("front", 110, 150, "ground", (0, 0, 0))]),
    "sd_default": ((0.0548929, 0.0876593, 0.145341), [
        ("up", 80, 80, "sky", (0.0274738, 0.0478771, 0.0940756)),
        ("up", 30, 80, "sky", (0.122738, 0.192101, 0.274424)),
        ("up", 130, 80, "sky", (0.202179, 0.272143, 0.348857)),
        ("up", 80, 25, "sky", (0.115147, 0.175268, 0.251005)),
        ("up", 80, 135, "sky", (0.11617, 0.176336, 0.251758)),
        ("up", 20, 20, "sky", (0.145801, 0.211142, 0.279666)),
        ("up", 140, 140, "sky", (0.178726, 0.241007, 0.304949)),
        ("front", 20, 80, "sky", (0.137212, 0.161077, 0.200626)),
        ("front", 50, 30, "sky", (0.0774128, 0.123821, 0.196796)),
        ("front", 65, 130, "sky", (0.11522, 0.175336, 0.256267)),
        ("front", 75, 60, "sky", (0.209951, 0.276135, 0.342385)),
        ("front", 80, 40, "fade", (0.000493637, 0.000585898, 0.000597909)),
        ("front", 95, 20, "ground", (0, 0, 0)),
        ("front", 110, 150, "ground", (0, 0, 0))]),
    "sd_ground": ((0.0691622, 0.0502981, 0.0790451), [
        ("up", 80, 80, "sky", (0.0286066, 0.0206269, 0.0449766)),
        ("up", 30, 80, "sky", (0.187537, 0.126952, 0.167537)),
        ("up", 130, 80, "sky", (0.554951, 0.438089, 0.336025)),
        ("up", 80, 25, "sky", (0.166794, 0.0884374, 0.125589)),
        ("up", 80, 135, "sky", (0.168135, 0.0888401, 0.125729)),
        ("up", 20, 20, "sky", (0.21386, 0.121657, 0.150869)),
        ("up", 140, 140, "sky", (0.26432, 0.161182, 0.170785)),
        ("front", 20, 80, "sky", (0.0564949, 0.0455037, 0.0739829)),
        ("front", 50, 30, "sky", (0.101354, 0.0756055, 0.111814)),
        ("front", 65, 130, "sky", (0.165114, 0.113274, 0.148193)),
        ("front", 75, 60, "sky", (0.390887, 0.279625, 0.245987)),
        ("front", 80, 40, "fade", (0.3, 0.2, 0.1)),
        ("front", 95, 20, "ground", (0.3, 0.2, 0.1)),
        ("front", 110, 150, "ground", (0.3, 0.2, 0.1))]),
    "sd_light": ((0.155247, 0.173544, 0.164426), [
        ("up", 80, 80, "sky", (0.0777077, 0.0947918, 0.106435)),
        ("up", 30, 80, "sky", (0.347155, 0.38034, 0.310475)),
        ("up", 130, 80, "sky", (0.571847, 0.538816, 0.394687)),
        ("up", 80, 25, "sky", (0.325685, 0.347014, 0.283979)),
        ("up", 80, 135, "sky", (0.328578, 0.349127, 0.284831)),
        ("up", 20, 20, "sky", (0.412388, 0.41804, 0.316405)),
        ("up", 140, 140, "sky", (0.505513, 0.47717, 0.34501)),
        ("front", 20, 80, "sky", (0.388095, 0.318918, 0.226983)),
        ("front", 50, 30, "sky", (0.218957, 0.245154, 0.222649)),
        ("front", 65, 130, "sky", (0.325891, 0.347147, 0.289933)),
        ("front", 75, 60, "sky", (0.593832, 0.546721, 0.387364)),
        ("front", 80, 40, "fade", (0.00139622, 0.00116002, 0.000676457)),
        ("front", 95, 20, "ground", (0, 0, 0)),
        ("front", 110, 150, "ground", (0, 0, 0))]),
    "sd_noblur": ((0.0426077, 0.062089, 0.0895736), [
        ("up", 80, 80, "sky", (0.015048, 0.0263813, 0.0513575)),
        ("up", 30, 80, "sky", (0.11188, 0.161526, 0.191932)),
        ("up", 130, 80, "sky", (0.484439, 0.472193, 0.364922)),
        ("up", 80, 25, "sky", (0.0887648, 0.123915, 0.150025)),
        ("up", 80, 135, "sky", (0.0895229, 0.124593, 0.150286)),
        ("up", 20, 20, "sky", (0.123244, 0.163286, 0.177881)),
        ("up", 140, 140, "sky", (0.173678, 0.202851, 0.198615)),
        ("front", 20, 80, "sky", (0.0377463, 0.0537266, 0.0823249)),
        ("front", 50, 30, "sky", (0.0633758, 0.0926175, 0.126072)),
        ("front", 65, 130, "sky", (0.104052, 0.140945, 0.169161)),
        ("front", 75, 60, "sky", (0.305025, 0.319895, 0.274974)),
        ("front", 80, 40, "fade", (0.3, 0.2, 0.1)),
        ("front", 95, 20, "ground", (0.3, 0.2, 0.1)),
        ("front", 110, 150, "ground", (0.3, 0.2, 0.1))]),
    "sd_rotated": ((0.0627237, 0.0876874, 0.124839), [
        ("up", 80, 80, "sky", (0.0267116, 0.0434783, 0.0793148)),
        ("up", 30, 80, "sky", (0.0572043, 0.0972343, 0.152898)),
        ("up", 130, 80, "ground", (0, 0, 0)),
        ("up", 80, 25, "ground", (0, 0, 0)),
        ("up", 80, 135, "sky", (0.046889, 0.0826912, 0.135433)),
        ("up", 20, 20, "fade", (0, 0, 0)),
        ("up", 140, 140, "sky", (0.0863915, 0.134057, 0.188958)),
        ("front", 20, 80, "sky", (0.0499732, 0.0767585, 0.120894)),
        ("front", 50, 30, "sky", (0.339916, 0.378183, 0.362029)),
        ("front", 65, 130, "sky", (0.0581058, 0.0976779, 0.153013)),
        ("front", 75, 60, "ground", (0, 0, 0)),
        ("front", 80, 40, "fade", (0, 0, 0)),
        ("front", 95, 20, "ground", (0, 0, 0)),
        ("front", 110, 150, "ground", (0, 0, 0))]),
}


def read_rgb(oiio, np, path):
    image = oiio.ImageInput.open(path)
    if not image:
        return None
    spec = image.spec()
    px = np.asarray(image.read_image(0, 0, 0, spec.nchannels, oiio.FLOAT)).reshape(spec.height, spec.width, -1)
    image.close()
    return px[..., :3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--hfs", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    os.add_dll_directory(os.path.join(args.hfs, "bin"))
    import numpy as np
    import OpenImageIO as oiio

    env, prefs = isolated_env(args)
    for name, (patch, pixels) in KARMA.items():
        renderers = [("cpu", "Nova")]
        if name.startswith("ps_"):
            renderers.append(("xpu", "HdNovaXpuRendererPlugin"))
        for tag, renderer in renderers:
            images = {}
            for view in ("up", "front"):
                exr = os.path.join(args.out, f"{name}_{view}_{tag}.exr")
                proc = run([tool(args, "husk.exe"), "--renderer", renderer, "--headlight", "none", "-s",
                            "/Render/" + view, "-o", exr, os.path.join(SCENES, name + ".usda")], env, args.out)
                check(proc.returncode == 0, f"husk {name}.usda {view} ({tag}) exits with 0")
                check("does not support" not in proc.stderr,
                      f"{name} {view} ({tag}): no unsupported-light warning")
                images[view] = read_rgb(oiio, np, exr)
            if any(img is None for img in images.values()):
                check(False, f"{name} ({tag}): images missing")
                continue
            for view, row, col, kind, karma in pixels:
                nova = images[view][row, col]
                rel, ab = TOL[kind]
                worst = max(abs(float(n) - k) - (rel * abs(k) + ab) for n, k in zip(nova, karma))
                check(worst <= 0.0, f"{name} {view} ({row}, {col}) {kind} ({tag}): Nova "
                                    f"{tuple(round(float(v), 5) for v in nova)}, Karma {karma}")
            mean = images["front"][PATCH].reshape(-1, 3).mean(axis=0)
            ratio = [float(m) / k for m, k in zip(mean, patch)]
            check(all(abs(r - 1.0) <= 0.02 for r in ratio),
                  f"{name} irradiance ({tag}): Nova/Karma {tuple(round(r, 4) for r in ratio)}")

    # Light linking: sd_default with the sky's light link excluding the quad
    # (the generic light goes through the light-linking scene index). Karma:
    # the quad black, the sky unchanged (front (20, 80): 0.13721, 0.16108,
    # 0.20063).
    with open(os.path.join(SCENES, "sd_default.usda"), encoding="utf-8") as f:
        text = f.read()
    linked = text.replace('apiSchemas = ["HoudiniViewportGuideAPI", "KarmaLightAPI"]',
                          'apiSchemas = ["HoudiniViewportGuideAPI", "KarmaLightAPI", "CollectionAPI:lightLink"]', 1)
    linked = linked.replace("bool inputs:karma:light:renderlightgeo = 1",
                            "bool inputs:karma:light:renderlightgeo = 1\n"
                            "        rel collection:lightLink:excludes = </rig/patch>", 1)
    check('"CollectionAPI:lightLink"' in linked and "collection:lightLink:excludes" in linked,
          "light-link variant of sd_default written")
    usd = os.path.join(args.out, "sd_link.usda")
    with open(usd, "w", encoding="utf-8", newline="\n") as f:
        f.write(linked)
    exr = os.path.join(args.out, "sd_link_front.exr")
    proc = run([tool(args, "husk.exe"), "--renderer", "Nova", "--headlight", "none", "-s", "/Render/front", "-o",
                exr, usd], env, args.out)
    check(proc.returncode == 0, "husk sd_link.usda exits with 0")
    img = read_rgb(oiio, np, exr)
    if img is not None:
        quad = float(img[PATCH].max())
        check(quad == 0.0, f"a sky linked away from the quad leaves it black (max {quad:.3g}; Karma 0)")
        sky = [float(v) for v in img[20, 80]]
        check(all(abs(n - k) <= 0.02 * k for n, k in zip(sky, (0.13721, 0.16108, 0.20063))),
              f"the linked sky is still seen by the camera: {tuple(round(v, 5) for v in sky)}")
    shutil.rmtree(prefs, ignore_errors=True)
    print(f"{len(FAILURES)} failure(s)")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
