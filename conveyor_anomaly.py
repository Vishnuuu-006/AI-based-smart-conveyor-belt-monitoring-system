import argparse
import csv
import json
import random
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image
from sklearn.metrics import (
    average_precision_score,
    classification_report,
    confusion_matrix,
    roc_auc_score,
)
from torch import nn
from torch.utils.data import Dataset, DataLoader
from torchvision import models, transforms
from tqdm import tqdm


IMAGE_EXTENSIONS = {
    ".jpg", ".jpeg", ".png", ".bmp",
    ".tif", ".tiff", ".webp"
}

MEAN = [0.485, 0.456, 0.406]
STD = [0.229, 0.224, 0.225]


def set_seed(seed=42):
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)

    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)

    torch.backends.cudnn.benchmark = False
    torch.backends.cudnn.deterministic = True


def get_transform(image_size):
    return transforms.Compose([
        transforms.Resize((image_size, image_size)),
        transforms.ToTensor(),
        transforms.Normalize(MEAN, STD),
    ])


class ImageFolderDataset(Dataset):
    """Recursively loads images without requiring class subfolders."""

    def __init__(self, folder, image_size=224):
        self.folder = Path(folder)

        if not self.folder.is_dir():
            raise ValueError(f"Folder not found: {self.folder}")

        self.paths = sorted(
            path
            for path in self.folder.rglob("*")
            if path.is_file()
            and path.suffix.lower() in IMAGE_EXTENSIONS
        )

        if not self.paths:
            raise ValueError(f"No images found in {self.folder}")

        self.transform = get_transform(image_size)

    def __len__(self):
        return len(self.paths)

    def __getitem__(self, index):
        path = self.paths[index]

        try:
            with Image.open(path) as image:
                tensor = self.transform(image.convert("RGB"))
        except Exception as error:
            raise RuntimeError(
                f"Could not read image: {path}"
            ) from error

        return tensor, str(path)


def make_loader(folder, image_size, batch_size, workers, device):
    dataset = ImageFolderDataset(folder, image_size)

    return DataLoader(
        dataset,
        batch_size=batch_size,
        shuffle=False,
        num_workers=workers,
        pin_memory=(device.type == "cuda"),
        drop_last=False,
    )


class PatchFeatureExtractor(nn.Module):
    def __init__(self, pretrained=True):
        super().__init__()

        weights = (
            models.ResNet18_Weights.DEFAULT
            if pretrained else None
        )

        backbone = models.resnet18(weights=weights)

        self.stem = nn.Sequential(
            backbone.conv1,
            backbone.bn1,
            backbone.relu,
            backbone.maxpool,
        )

        self.layer1 = backbone.layer1
        self.layer2 = backbone.layer2
        self.layer3 = backbone.layer3

        for parameter in self.parameters():
            parameter.requires_grad = False

    def forward(self, images):
        x = self.stem(images)
        x = self.layer1(x)

        feature2 = self.layer2(x)
        feature3 = self.layer3(feature2)

        # Aggregate local neighborhoods.
        feature2 = F.avg_pool2d(
            feature2, kernel_size=3, stride=1, padding=1
        )
        feature3 = F.avg_pool2d(
            feature3, kernel_size=3, stride=1, padding=1
        )

        # Align spatial resolutions.
        feature3 = F.interpolate(
            feature3,
            size=feature2.shape[-2:],
            mode="bilinear",
            align_corners=False,
        )

        features = torch.cat([feature2, feature3], dim=1)

        # [B, C, H, W] -> [B, H*W, C]
        patches = features.flatten(2).transpose(1, 2)

        return patches.contiguous()


@torch.inference_mode()
def build_memory_bank(
    model,
    loader,
    device,
    memory_size,
    patches_per_image,
):
    bank = None
    priorities = None

    for images, _ in tqdm(loader, desc="Building normal memory"):
        images = images.to(device, non_blocking=True)

        # Shape: [batch, patches, feature_dimensions]
        features = model(images)

        batch_size, patch_count, feature_dim = features.shape
        sample_count = min(patches_per_image, patch_count)

        # Randomly select patches independently for each image.
        random_values = torch.rand(
            batch_size, patch_count, device=device
        )

        selected_indices = random_values.topk(
            sample_count, dim=1
        ).indices

        sampled = features.gather(
            1,
            selected_indices.unsqueeze(-1).expand(
                -1, -1, feature_dim
            ),
        )

        sampled = sampled.reshape(-1, feature_dim).cpu()
        new_priorities = torch.rand(len(sampled))

        if bank is None:
            bank = sampled
            priorities = new_priorities
        else:
            bank = torch.cat([bank, sampled], dim=0)
            priorities = torch.cat(
                [priorities, new_priorities], dim=0
            )

        # Retain a bounded random subset of sampled patches.
        if len(bank) > memory_size:
            keep = priorities.topk(memory_size).indices
            bank = bank[keep]
            priorities = priorities[keep]

    return bank.contiguous()


@torch.inference_mode()
def batch_anomaly_scores(
    model,
    images,
    memory_bank,
    distance_chunk=256,
):
    features = model(images)

    batch_size, patch_count, feature_dim = features.shape
    flattened = features.reshape(-1, feature_dim)

    nearest_distances = []

    # Limit intermediate distance-matrix size.
    for chunk in flattened.split(distance_chunk):
        distances = torch.cdist(
            chunk.unsqueeze(0),
            memory_bank.unsqueeze(0),
            p=2,
        ).squeeze(0)

        nearest_distances.append(
            distances.min(dim=1).values
        )

    patch_scores = torch.cat(nearest_distances)
    patch_scores = patch_scores.reshape(batch_size, patch_count)

    # Image score = largest local deviation from normal memory.
    return patch_scores.max(dim=1).values


@torch.inference_mode()
def score_loader(model, loader, memory_bank, device):
    all_paths = []
    all_scores = []

    for images, paths in tqdm(loader, desc="Scoring images"):
        images = images.to(device, non_blocking=True)

        scores = batch_anomaly_scores(
            model, images, memory_bank
        )

        all_paths.extend(paths)
        all_scores.extend(scores.cpu().tolist())

    return all_paths, np.asarray(all_scores, dtype=np.float64)


def save_score_csv(path, image_paths, scores):
    with open(path, "w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["image", "anomaly_score"])

        for image_path, score in zip(image_paths, scores):
            writer.writerow([image_path, float(score)])


def train(args, device):
    if not 0 < args.quantile < 1:
        raise ValueError("--quantile must be between 0 and 1.")

    if args.memory_size < 1 or args.patches_per_image < 1:
        raise ValueError("Memory and patch counts must be positive.")

    train_loader = make_loader(
        args.normal_train,
        args.image_size,
        args.batch_size,
        args.workers,
        device,
    )

    val_loader = make_loader(
        args.normal_val,
        args.image_size,
        args.batch_size,
        args.workers,
        device,
    )

    print("Normal training images:", len(train_loader.dataset))
    print("Normal validation images:", len(val_loader.dataset))

    model = PatchFeatureExtractor(pretrained=True).to(device)
    model.eval()

    memory_bank = build_memory_bank(
        model,
        train_loader,
        device,
        args.memory_size,
        args.patches_per_image,
    )

    print("Memory bank shape:", tuple(memory_bank.shape))

    val_paths, val_scores = score_loader(
        model,
        val_loader,
        memory_bank.to(device),
        device,
    )

    threshold = float(
        np.quantile(val_scores, args.quantile)
    )

    model_path = Path(args.model)
    model_path.parent.mkdir(parents=True, exist_ok=True)

    checkpoint = {
        "extractor_state": {
            name: tensor.detach().cpu()
            for name, tensor in model.state_dict().items()
        },
        "memory_bank": memory_bank.cpu(),
        "threshold": threshold,
        "threshold_quantile": args.quantile,
        "image_size": args.image_size,
        "method": "ResNet18 random-memory PatchCore-style",
        "training_image_count": len(train_loader.dataset),
        "validation_image_count": len(val_loader.dataset),
    }

    torch.save(checkpoint, model_path)

    save_score_csv(
        model_path.parent / "normal_validation_scores.csv",
        val_paths,
        val_scores,
    )

    summary = {
        "training_images": len(train_loader.dataset),
        "validation_images": len(val_loader.dataset),
        "memory_bank_size": len(memory_bank),
        "threshold": threshold,
        "threshold_quantile": args.quantile,
        "validation_score_min": float(val_scores.min()),
        "validation_score_median": float(np.median(val_scores)),
        "validation_score_max": float(val_scores.max()),
    }

    with open(
        model_path.parent / "training_summary.json", "w"
    ) as file:
        json.dump(summary, file, indent=2)

    print("\nModel construction complete.")
    print(f"Threshold: {threshold:.6f}")
    print("Saved model:", model_path)


def load_model(model_path, device):
    # Load only checkpoints you trust.
    checkpoint = torch.load(
        model_path,
        map_location="cpu",
        weights_only=True,
    )

    model = PatchFeatureExtractor(pretrained=False)
    model.load_state_dict(checkpoint["extractor_state"])
    model = model.to(device)
    model.eval()

    memory_bank = checkpoint["memory_bank"].to(device)
    threshold = float(checkpoint["threshold"])
    image_size = int(checkpoint["image_size"])

    return model, memory_bank, threshold, image_size


def evaluate(args, device):
    model, memory_bank, threshold, image_size = load_model(
        args.model, device
    )

    normal_loader = make_loader(
        args.normal_test,
        image_size,
        args.batch_size,
        args.workers,
        device,
    )

    anomaly_loader = make_loader(
        args.anomaly_test,
        image_size,
        args.batch_size,
        args.workers,
        device,
    )

    normal_paths, normal_scores = score_loader(
        model, normal_loader, memory_bank, device
    )

    anomaly_paths, anomaly_scores = score_loader(
        model, anomaly_loader, memory_bank, device
    )

    # 0 = normal; 1 = anomaly.
    y_true = np.concatenate([
        np.zeros(len(normal_scores), dtype=int),
        np.ones(len(anomaly_scores), dtype=int),
    ])

    scores = np.concatenate([normal_scores, anomaly_scores])
    predictions = (scores > threshold).astype(int)

    matrix = confusion_matrix(
        y_true, predictions, labels=[0, 1]
    )

    report = classification_report(
        y_true,
        predictions,
        labels=[0, 1],
        target_names=["normal", "anomaly"],
        output_dict=True,
        zero_division=0,
    )

    metrics = {
        "threshold": threshold,
        "normal_test_images": len(normal_scores),
        "anomaly_test_images": len(anomaly_scores),
        "roc_auc": float(roc_auc_score(y_true, scores)),
        "average_precision": float(
            average_precision_score(y_true, scores)
        ),
        "false_alarm_rate_per_normal_image": float(
            np.mean(normal_scores > threshold)
        ),
        "anomaly_recall": float(
            np.mean(anomaly_scores > threshold)
        ),
        "confusion_matrix": matrix.tolist(),
        "classification_report": report,
    }

    output_dir = Path(args.out)
    output_dir.mkdir(parents=True, exist_ok=True)

    with open(output_dir / "test_metrics.json", "w") as file:
        json.dump(metrics, file, indent=2)

    with open(
        output_dir / "test_predictions.csv",
        "w",
        newline="",
        encoding="utf-8",
    ) as file:
        writer = csv.writer(file)
        writer.writerow([
            "image", "actual", "predicted", "anomaly_score"
        ])

        for path, actual, predicted, score in zip(
            normal_paths + anomaly_paths,
            y_true,
            predictions,
            scores,
        ):
            writer.writerow([
                path,
                "anomaly" if actual else "normal",
                "anomaly" if predicted else "normal",
                float(score),
            ])

    print("\nTEST CLASSIFICATION REPORT")
    print(classification_report(
        y_true,
        predictions,
        labels=[0, 1],
        target_names=["normal", "anomaly"],
        digits=4,
        zero_division=0,
    ))

    print("Confusion matrix: rows=actual, columns=predicted")
    print("Class order: normal, anomaly")
    print(matrix)

    print("\nROC-AUC:", metrics["roc_auc"])
    print("Average precision:", metrics["average_precision"])
    print("Anomaly recall:", metrics["anomaly_recall"])
    print(
        "Normal-image false-alarm rate:",
        metrics["false_alarm_rate_per_normal_image"],
    )
    print("Saved results:", output_dir)


@torch.inference_mode()
def predict(args, device):
    model, memory_bank, threshold, image_size = load_model(
        args.model, device
    )

    with Image.open(args.image) as image:
        tensor = get_transform(image_size)(
            image.convert("RGB")
        )

    tensor = tensor.unsqueeze(0).to(device)

    score = float(
        batch_anomaly_scores(model, tensor, memory_bank)[0].item()
    )

    decision = "ANOMALY" if score > threshold else "NORMAL"

    print("\nPrediction:", decision)
    print(f"Anomaly score: {score:.6f}")
    print(f"Threshold: {threshold:.6f}")

    if decision == "ANOMALY":
        print("Possible foreign object or other visual change.")

    print("The score is a distance, not a probability.")


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "mode", choices=["train", "evaluate", "predict"]
    )

    parser.add_argument(
        "--model", default="outputs/conveyor_anomaly.pt"
    )
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--workers", type=int, default=0)
    parser.add_argument("--cpu", action="store_true")

    parser.add_argument("--normal-train")
    parser.add_argument("--normal-val")
    parser.add_argument("--normal-test")
    parser.add_argument("--anomaly-test")
    parser.add_argument("--image")

    parser.add_argument("--image-size", type=int, default=224)
    parser.add_argument("--memory-size", type=int, default=10000)
    parser.add_argument(
        "--patches-per-image", type=int, default=50
    )
    parser.add_argument("--quantile", type=float, default=0.99)
    parser.add_argument("--out", default="outputs/evaluation")

    args = parser.parse_args()

    if args.batch_size < 1:
        parser.error("--batch-size must be positive.")

    required = {
        "train": ["normal_train", "normal_val"],
        "evaluate": ["normal_test", "anomaly_test"],
        "predict": ["image"],
    }

    for name in required[args.mode]:
        if getattr(args, name) is None:
            parser.error(
                f"{args.mode} requires --{name.replace('_', '-')}"
            )

    set_seed(42)

    device = torch.device(
        "cuda"
        if torch.cuda.is_available() and not args.cpu
        else "cpu"
    )

    print("Device:", device)

    if args.mode == "train":
        train(args, device)
    elif args.mode == "evaluate":
        evaluate(args, device)
    else:
        predict(args, device)


if __name__ == "__main__":
    main()
