#!/usr/bin/env python3
"""
Streaming Causal CNN Drum Detector — 训练 + 导出 C++ 权重
============================================================

模型架构（必须与 C++ 侧 CNNDrumDetector.h/cpp 完全一致）：
    Input:  [T=15][C=80]  log-mel
    Conv1d(80 → 20, kernel=3, valid)  + ReLU
    Conv1d(20 → 20, kernel=3, valid)  + ReLU
    Flatten(220)
    Linear(220 → 32) + ReLU
    Linear(32 → 3)   + Sigmoid → [kick, snare, hihat]

数据源 (推荐)：
    - MDB-Drums:  https://github.com/CarlSouthall/MDBDrums
    - ENST-Drums: http://perso.telecom-paristech.fr/grichard/ENST-drums/
    - IDMT-SMT-Drums: https://www.idmt.fraunhofer.de/en/publications/datasets/drums.html

若 --synth 开关开启，则合成伪数据训练（不如真实数据但可以快速验证端到端）。

用法：
    # 安装依赖（一次）
    pip install torch torchaudio librosa numpy

    # 真实数据训练
    python train_drum_cnn.py --data /path/to/MDB-Drums --epochs 30 \\
        --out ../src/effect/CNNDrumWeights.cpp

    # 合成数据快速测试（默认也会有一定泛化）
    python train_drum_cnn.py --synth --epochs 20 \\
        --out ../src/effect/CNNDrumWeights.cpp

导出的 CNNDrumWeights.cpp 把 CNN_MODEL_TRAINED 置为 true，替换原占位文件即可。
"""
import argparse
import os
import sys
import random
import numpy as np

try:
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
    from torch.utils.data import Dataset, DataLoader
except ImportError:
    sys.exit("请先 pip install torch torchaudio")

try:
    import librosa
except ImportError:
    print("警告：librosa 未安装，--data 模式无法读取音频。pip install librosa")
    librosa = None


# ─── 与 C++ 侧常量保持一致 ─────────────────────────────────────────
NUM_MEL         = 80
CONTEXT_FRAMES  = 15
CONV1_OUT       = 20
CONV2_OUT       = 20
CONV_KERNEL     = 3
DENSE_HIDDEN    = 32
NUM_CLASSES     = 3         # kick / snare / hihat

SAMPLE_RATE     = 48000
FFT_SIZE        = 1024
HOP_SIZE        = 512       # 10.67ms at 48kHz
MEL_FMIN        = 0.0
MEL_FMAX        = SAMPLE_RATE / 2

# mel 能量压缩系数（与 C++ applyLog 中 log(1+100·mel) 一致）
LOG_MEL_LAMBDA  = 100.0


# ════════════════════════════════════════════════════════════════
# 1. 模型
# ════════════════════════════════════════════════════════════════
class CausalDrumCNN(nn.Module):
    """和 C++ 侧数值完全等价的 causal CNN。"""
    def __init__(self):
        super().__init__()
        # PyTorch Conv1d 期望 (B, C, T)，我们的 mel 数据是 (B, T, C) 要先 transpose
        self.conv1 = nn.Conv1d(NUM_MEL,    CONV1_OUT, kernel_size=CONV_KERNEL, padding=0)
        self.conv2 = nn.Conv1d(CONV1_OUT, CONV2_OUT, kernel_size=CONV_KERNEL, padding=0)
        # 每层 valid conv 时间维减 2，两层后 15-4=11
        flatten_dim = CONV2_OUT * (CONTEXT_FRAMES - 2 * (CONV_KERNEL - 1))
        self.fc1 = nn.Linear(flatten_dim, DENSE_HIDDEN)
        self.fc2 = nn.Linear(DENSE_HIDDEN, NUM_CLASSES)

    def forward(self, x):
        # 示例/字段：x: (B, T, C) → (B, C, T)
        x = x.transpose(1, 2)
        x = F.relu(self.conv1(x))
        x = F.relu(self.conv2(x))
        x = x.transpose(1, 2).reshape(x.size(0), -1)
        x = F.relu(self.fc1(x))
        return torch.sigmoid(self.fc2(x))


# ════════════════════════════════════════════════════════════════
# 2. 数据准备
# ════════════════════════════════════════════════════════════════
def compute_log_mel(y, sr=SAMPLE_RATE):
    """从 waveform 计算 (T, NUM_MEL) log-mel 矩阵，与 C++ 侧 MelFilterbank 一致。"""
    S = librosa.feature.melspectrogram(
        y=y, sr=sr, n_fft=FFT_SIZE, hop_length=HOP_SIZE,
        n_mels=NUM_MEL, fmin=MEL_FMIN, fmax=MEL_FMAX,
        htk=True,     # C++ 侧用 HTK mel 公式
        norm='slaney', power=2.0)  # librosa 默认 slaney 归一化
    return np.log1p(LOG_MEL_LAMBDA * S.T)   # (T, NUM_MEL)


def gen_synthetic_clip(duration_sec=8.0):
    """合成一段假音频：kick/snare/hihat 以一定节奏出现，背景粉噪。

    返回：(waveform[T], onsets) where onsets = list of (time_sec, label_idx)
          label_idx: 0=kick 1=snare 2=hihat
    """
    sr = SAMPLE_RATE
    N = int(duration_sec * sr)
    y = np.random.randn(N).astype(np.float32) * 0.005    # 弱白噪背景
    onsets = []
    bpm = random.uniform(95, 160)
    beat_t = 60.0 / bpm
    t = 0.2
    beat_idx = 0
    while t < duration_sec - 0.2:
        # kick 落在 4/4 拍第 1、3 拍
        if beat_idx % 2 == 0:
            _paste_drum_hit(y, t, sr, 'kick')
            onsets.append((t, 0))
        # snare 落在第 2、4 拍
        if beat_idx % 2 == 1:
            _paste_drum_hit(y, t, sr, 'snare')
            onsets.append((t, 1))
        # hihat 落在每个八分音符上
        for h in [0, 0.5]:
            th = t + h * beat_t
            if th < duration_sec - 0.1:
                _paste_drum_hit(y, th, sr, 'hihat')
                onsets.append((th, 2))
        t += beat_t
        beat_idx += 1
    # 添加一个低频贝斯旋律（干扰源）
    for i in range(int(duration_sec * 2)):
        start = int(i * sr / 2)
        end = min(N, start + int(sr * 0.4))
        freq = random.choice([55, 65, 82, 98])
        bass = 0.15 * np.sin(2 * np.pi * freq * np.arange(end - start) / sr)
        y[start:end] += bass.astype(np.float32)
    return y, onsets


def _paste_drum_hit(y, t, sr, kind):
    """在 waveform y 的 t 秒处贴一个合成鼓声。"""
    start = int(t * sr)
    if kind == 'kick':
        dur = int(0.15 * sr)
        t_arr = np.arange(dur) / sr
        f = 60 * np.exp(-t_arr * 30)  # 60Hz → 快速衰减到很低
        env = np.exp(-t_arr * 15)
        hit = (env * np.sin(2 * np.pi * np.cumsum(f) / sr)).astype(np.float32) * 0.9
    elif kind == 'snare':
        dur = int(0.1 * sr)
        t_arr = np.arange(dur) / sr
        body = 0.4 * np.sin(2 * np.pi * 200 * t_arr) * np.exp(-t_arr * 30)
        noise = 0.6 * np.random.randn(dur) * np.exp(-t_arr * 25)
        hit = (body + noise).astype(np.float32) * 0.7
    else:  # hihat
        dur = int(0.05 * sr)
        t_arr = np.arange(dur) / sr
        noise = np.random.randn(dur) * np.exp(-t_arr * 80)
        # 6-10kHz bandpass 粗糙模拟
        from scipy.signal import butter, sosfilt
        sos = butter(4, [6000, 10000], btype='bandpass', fs=sr, output='sos')
        hit = sosfilt(sos, noise).astype(np.float32) * 0.4
    end = min(len(y), start + len(hit))
    y[start:end] += hit[:end - start]


class DrumDataset(Dataset):
    """每个样本 = 一个 15 帧上下文窗口 + 三通道 one-hot 标签。

    负样本（没鼓）占 80%，正样本在对应鼓击 onset 时刻生成。
    """
    def __init__(self, clips, onset_labels, pos_ratio=0.2):
        """clips: [np.array(log_mel[T, C])]
           onset_labels: [[(frame_idx, class_idx), ...]]"""
        self.samples = []  # list of (clip_idx, frame_idx, one_hot_label)
        for ci, (clip, onsets) in enumerate(zip(clips, onset_labels)):
            T = clip.shape[0]
            # 正样本：onset 对应的帧
            pos_frames = {}
            for fidx, cls in onsets:
                if CONTEXT_FRAMES - 1 <= fidx < T:
                    if fidx not in pos_frames:
                        pos_frames[fidx] = [0, 0, 0]
                    pos_frames[fidx][cls] = 1.0
            for fidx, lbl in pos_frames.items():
                self.samples.append((ci, fidx, np.array(lbl, dtype=np.float32)))
            # 负样本：不在任何 onset ±2 帧内的随机帧
            all_onset_frames = set()
            for f, _ in onsets:
                for d in range(-2, 3):
                    all_onset_frames.add(f + d)
            neg_count = int(len(pos_frames) * (1 - pos_ratio) / max(1e-6, pos_ratio))
            for _ in range(neg_count):
                f = random.randint(CONTEXT_FRAMES - 1, T - 1)
                if f not in all_onset_frames:
                    self.samples.append((ci, f, np.zeros(3, dtype=np.float32)))
        self.clips = clips
        random.shuffle(self.samples)

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, i):
        ci, fidx, lbl = self.samples[i]
        clip = self.clips[ci]
        window = clip[fidx - CONTEXT_FRAMES + 1: fidx + 1]  # (15, 80)
        return torch.from_numpy(window).float(), torch.from_numpy(lbl)


# ════════════════════════════════════════════════════════════════
# 3. 训练
# ════════════════════════════════════════════════════════════════
def train(model, loader, epochs, lr, device):
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    loss_fn = nn.BCELoss()
    model.train()
    for ep in range(epochs):
        total, correct, loss_sum = 0, 0, 0.0
        for x, y in loader:
            x, y = x.to(device), y.to(device)
            p = model(x)
            loss = loss_fn(p, y)
            opt.zero_grad()
            loss.backward()
            opt.step()
            loss_sum += loss.item() * x.size(0)
            # 粗略"正确率"：把 >0.5 当正类
            correct += ((p > 0.5) == (y > 0.5)).all(dim=1).sum().item()
            total += x.size(0)
        print(f"Epoch {ep+1}/{epochs}  loss={loss_sum/total:.4f}  acc={correct/total:.3f}")


# ════════════════════════════════════════════════════════════════
# 4. 导出为 C++ 权重
# ════════════════════════════════════════════════════════════════
def export_cpp(model, out_path):
    """把 PyTorch 权重写成 CNNDrumWeights.cpp"""
    sd = model.state_dict()
    conv1_w = sd['conv1.weight'].detach().cpu().numpy()  # (CONV1_OUT, NUM_MEL, K)
    conv1_b = sd['conv1.bias'].detach().cpu().numpy()
    conv2_w = sd['conv2.weight'].detach().cpu().numpy()
    conv2_b = sd['conv2.bias'].detach().cpu().numpy()
    fc1_w   = sd['fc1.weight'].detach().cpu().numpy()    # (DENSE_HIDDEN, FLATTEN)
    fc1_b   = sd['fc1.bias'].detach().cpu().numpy()
    fc2_w   = sd['fc2.weight'].detach().cpu().numpy()    # (NUM_CLASSES, DENSE_HIDDEN)
    fc2_b   = sd['fc2.bias'].detach().cpu().numpy()

    def arr4_to_cpp(name, arr, dims):
        """把多维 float 数组展平为 C++ 初始化列表"""
        flat = arr.reshape(-1)
        lines = [f"// 形状：{dims}"]
        items_per_line = 8
        chunks = []
        for i in range(0, len(flat), items_per_line):
            piece = ", ".join(f"{v:.7f}f" for v in flat[i:i + items_per_line])
            chunks.append("    " + piece)
        body = ",\n".join(chunks)
        return f"const float {name}{dims} = {{\n{body}\n}};\n"

    out = []
    out.append('// 由 scripts/train_drum_cnn.py 自动生成，请勿手动编辑。\n')
    out.append('#include "effect/CNNDrumWeights.h"\n\n')
    out.append('namespace hsvj {\n\n')

    # 注意：C++ 端的声明是 const float CNN_CONV1_W[20][80][3]
    # PyTorch conv1.weight shape = (out=20, in=80, k=3) — 完全一致
    out.append(arr4_to_cpp("CNN_CONV1_W", conv1_w,
                           f"[{CONV1_OUT}][{NUM_MEL}][{CONV_KERNEL}]"))
    out.append(arr4_to_cpp("CNN_CONV1_B", conv1_b, f"[{CONV1_OUT}]"))
    out.append(arr4_to_cpp("CNN_CONV2_W", conv2_w,
                           f"[{CONV2_OUT}][{CONV1_OUT}][{CONV_KERNEL}]"))
    out.append(arr4_to_cpp("CNN_CONV2_B", conv2_b, f"[{CONV2_OUT}]"))

    # Dense：PyTorch Linear.weight shape = (out, in)；和 C++ 端 [DENSE_HIDDEN][FLATTEN] 一致
    out.append(arr4_to_cpp("CNN_DENSE_HIDDEN_W", fc1_w,
                           f"[{DENSE_HIDDEN}][{fc1_w.shape[1]}]"))
    out.append(arr4_to_cpp("CNN_DENSE_HIDDEN_B", fc1_b, f"[{DENSE_HIDDEN}]"))
    out.append(arr4_to_cpp("CNN_DENSE_OUT_W", fc2_w,
                           f"[{NUM_CLASSES}][{DENSE_HIDDEN}]"))
    out.append(arr4_to_cpp("CNN_DENSE_OUT_B", fc2_b, f"[{NUM_CLASSES}]"))

    out.append('\n} // 命名空间 hsvj\n')

    # 同时要把头文件里的 CNN_MODEL_TRAINED 改为 true
    # 最简单方式：头文件用 constexpr，cpp 里额外定义一个 global flag；但为了单文件，
    # 我们在这里同时更新头文件。
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(''.join(out))
    print(f"✓ 已生成 {out_path}")

    # 把头文件的 CNN_MODEL_TRAINED 改为 true
    header_path = os.path.normpath(os.path.join(
        os.path.dirname(out_path),
        '..', '..', 'include', 'effect', 'CNNDrumWeights.h'))
    if os.path.exists(header_path):
        with open(header_path, 'r', encoding='utf-8') as f:
            content = f.read()
        new_content = content.replace(
            "inline constexpr bool CNN_MODEL_TRAINED = false;",
            "inline constexpr bool CNN_MODEL_TRAINED = true;")
        if new_content != content:
            with open(header_path, 'w', encoding='utf-8') as f:
                f.write(new_content)
            print(f"✓ 已将 {header_path} 里的 CNN_MODEL_TRAINED 置为 true")


# ════════════════════════════════════════════════════════════════
# 5. 主入口
# ════════════════════════════════════════════════════════════════
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--data', help='数据集根目录（含音频 + onset 标注）')
    parser.add_argument('--synth', action='store_true', help='用合成鼓声数据')
    parser.add_argument('--epochs', type=int, default=30)
    parser.add_argument('--batch', type=int, default=64)
    parser.add_argument('--lr', type=float, default=1e-3)
    parser.add_argument('--out', default='../src/effect/CNNDrumWeights.cpp')
    parser.add_argument('--synth-clips', type=int, default=80,
                        help='合成模式下生成多少段 8s clip')
    args = parser.parse_args()

    # 1) 准备数据
    clips, labels = [], []
    if args.synth:
        print(f"合成 {args.synth_clips} 段 8s 音频...")
        for i in range(args.synth_clips):
            y, onsets = gen_synthetic_clip()
            mel = compute_log_mel(y)
            # onset 时间 → frame idx
            frame_onsets = [(int(t * SAMPLE_RATE / HOP_SIZE), cls) for t, cls in onsets]
            clips.append(mel)
            labels.append(frame_onsets)
            if (i+1) % 10 == 0:
                print(f"  {i+1}/{args.synth_clips}")
    elif args.data:
        print(f"从 {args.data} 加载 MDB-Drums 数据集...")
        # MDB-Drums 结构：
        #   <root>/MDB Drums/音频/full_mix/<song>_MIX.wav
        #   <root>/MDB Drums/音频/drum_only/<song>_Drum.wav
        #   说明：<root>/MDB Drums/annotations/class/<song>_class.txt
        # 标注格式：每行 "<时间_sec>  <KD|SD|HH>"
        # 我们同时用 full_mix（真实场景）和 drum_only（纯鼓声，监督信号更纯）
        root = args.data
        # 兼容用户直接把仓库根目录传进来
        if os.path.isdir(os.path.join(root, "MDB Drums")):
            root = os.path.join(root, "MDB Drums")
        ann_dir = os.path.join(root, "annotations", "class")
        full_mix_dir = os.path.join(root, "audio", "full_mix")
        drum_only_dir = os.path.join(root, "audio", "drum_only")
        if not os.path.isdir(ann_dir):
            print(f"找不到 {ann_dir} —— 请确认 --data 指向 MDBDrums 根目录")
            sys.exit(1)

        import glob as _glob
        ann_files = sorted(_glob.glob(os.path.join(ann_dir, "*_class.txt")))
        print(f"发现 {len(ann_files)} 个歌曲")

        label_map = {"KD": 0, "SD": 1, "HH": 2}
        for i, ann_path in enumerate(ann_files):
            song = os.path.basename(ann_path).replace("_class.txt", "")
            # 解析 onset 标注
            onsets_sec = []
            with open(ann_path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) < 2:
                        continue
                    try:
                        t = float(parts[0])
                    except ValueError:
                        continue
                    cls = label_map.get(parts[1].upper())
                    if cls is None:
                        continue
                    onsets_sec.append((t, cls))
            if not onsets_sec:
                continue

            # 2 种音频：全混音 + 纯鼓。都用作训练样本（数据增强）
            for kind, wav_dir, suffix in (
                ("full_mix", full_mix_dir, "_MIX.wav"),
                ("drum_only", drum_only_dir, "_Drum.wav"),
            ):
                wav_path = os.path.join(wav_dir, song + suffix)
                if not os.path.exists(wav_path):
                    continue
                y, _sr = librosa.load(wav_path, sr=SAMPLE_RATE, mono=True)
                mel = compute_log_mel(y)
                T = mel.shape[0]
                frame_onsets = [(int(t * SAMPLE_RATE / HOP_SIZE), c)
                                for t, c in onsets_sec]
                # 过滤越界
                frame_onsets = [(f, c) for f, c in frame_onsets if f < T]
                clips.append(mel)
                labels.append(frame_onsets)
            print(f"  [{i+1}/{len(ann_files)}] {song}: {len(onsets_sec)} onsets")
    else:
        print("必须指定 --data 或 --synth")
        sys.exit(1)

    # 2) 建 DataLoader
    ds = DrumDataset(clips, labels)
    print(f"样本总数：{len(ds)}")
    loader = DataLoader(ds, batch_size=args.batch, shuffle=True)

    # 3) 训练
    device = 'cuda' if torch.cuda.is_available() else 'cpu'
    print(f"训练设备：{device}")
    model = CausalDrumCNN().to(device)
    train(model, loader, args.epochs, args.lr, device)

    # 4) 导出
    export_cpp(model, args.out)
    print("\n✓ 训练 + 导出完成。重新编译 C++ 项目即可启用 CNN 鼓声检测。")


if __name__ == '__main__':
    main()
