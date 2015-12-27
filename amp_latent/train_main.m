
%% Build Java
system('make clean')
system('make')
addpath('/home/sandip/matlab/libsvm-3.21/matlab');

%% Infer Latent Vars

%delete existing
system('rm tmp1/inf_lat_var_all.result')

%call java
system('./infer_latent_cmd.sh')

%reading java results
fileID = fopen('tmp1/inf_lat_var_all.result','r');
formatSpec = '%d';
latent_labels = fscanf(fileID,formatSpec);
[latent_size,~] = size(latent_labels)

%% Other Matrices (Hard Coded)

NO_OF_RELNS = 51+1;
NO_OF_FEATURES = 56648;
NO_OF_EXMPLS = 111;
feature_vect = rand(latent_size, NO_OF_FEATURES);

% Weight vector initialize
W = zeros(NO_OF_RELNS, NO_OF_FEATURES);
bias = zeros(NO_OF_RELNS, 1);

%% SVM Training

for i=1:30:50
    temp_reln_labels = zeros(latent_size,1);
    temp_reln_labels(latent_labels==i) = 1;
    model = svmtrain(temp_reln_labels, feature_vect, '-s 0 -t 2 -c 1 -g 0.1')
    
    % Need to verify next 2 lines  (taken from http://stackoverflow.com/questions/10131385/matlab-libsvm-how-to-find-the-w-coefficients)
    W(i+1,:) = (model.sv_coef' * full(model.SVs));
    bias(i+1,:)=-model.rho;
end
% temp_reln'

%% SVM Prediction

%Replicate bias to add with W
bias_repl=repmat(bias,1,latent_size);

%Prediction vector (need to be more verbose with comments)
[predictions_score,predictions_vect] = max(W * feature_vect' + bias_repl);
predictions_vect = predictions_vect-1;



%% Calculate TP & TN - Example

% a=[1 0 0 1 0 1 1]
% b=[1 0 0 0 0 1 0]
% TP = sum(a.*b)
% 
% c = zeros(1,7)
% d = zeros(1,7)
% c(a==0) = 1
% d(b==0) = 1
% TN = sum(c.*d)
































