% Projector-Camera Stereo calibration parameters:

% Intrinsic parameters of camera:
fc_left = [ 3598.307262 3588.039465 ]; % Focal Length
cc_left = [ 1239.280147 994.945799 ]; % Principal point
alpha_c_left = [ 0.000000 ]; % Skew
kc_left = [ -0.019089 -0.165434 -0.003274 -0.000691 0.000000 ]; % Distortion

% Intrinsic parameters of projector:
fc_right = [ 2113.996293 1582.221301 ]; % Focal Length
cc_right = [ 456.457401 711.217211 ]; % Principal point
alpha_c_right = [ 0.000000 ]; % Skew
kc_right = [ -0.022201 0.079758 -0.001127 -0.000575 0.000000 ]; % Distortion

% Extrinsic parameters (position of projector wrt camera):
om = [ 0.210163 0.284709 0.023911 ]; % Rotation vector
T = [ -253.397945 -16.870805 14.926718 ]; % Translation vector
