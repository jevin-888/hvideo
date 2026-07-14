package com.hsvj.ui;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.SystemClock;
import android.util.AttributeSet;
import android.view.Choreographer;
import android.view.View;
import android.view.animation.DecelerateInterpolator;

/**
 * 初始化进度加载覆盖层
 * 显示在引擎初始化过程中，提供视觉反馈
 */
public class LoadingOverlayView extends View {

    private int mCurrentProgress = 0;
    private String mCurrentMessage = "正在启动...";
    private float mAlpha = 1.0f;

    // 画笔对象
    private Paint mBgPaint;
    private Paint mTitlePaint;
    private Paint mMessagePaint;
    private Paint mProgressBgPaint;
    private Paint mProgressPaint;
    private Paint mSpinnerPaint;

    // 尺寸参数
    private float mStrokeWidth;
    private float mProgressBarHeight;
    private float mMaxProgressBarWidth;

    // 动画参数
    private ValueAnimator mProgressAnimator;
    private float mSpinnerAngle = 0f;
    private boolean mSpinnerRunning = false;
    private long mLastSpinnerFrameTimeMs = 0L;
    private final Choreographer.FrameCallback mSpinnerFrameCallback = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            if (!mSpinnerRunning) {
                return;
            }

            long nowMs = SystemClock.uptimeMillis();
            if (mLastSpinnerFrameTimeMs == 0L) {
                mLastSpinnerFrameTimeMs = nowMs;
            }

            long deltaMs = nowMs - mLastSpinnerFrameTimeMs;
            mLastSpinnerFrameTimeMs = nowMs;
            if (deltaMs < 0L) {
                deltaMs = 0L;
            }
            if (deltaMs > 100L) {
                deltaMs = 100L;
            }

            mSpinnerAngle = (mSpinnerAngle + (deltaMs * 0.36f)) % 360f;
            postInvalidateOnAnimation();
            Choreographer.getInstance().postFrameCallback(this);
        }
    };

    // 颜色参数
    private static final int BG_COLOR = Color.parseColor("#CC000000");
    private static final int TITLE_COLOR = Color.parseColor("#FFFFFF");
    private static final int MESSAGE_COLOR = Color.parseColor("#9CA3AF");
    private static final int PROGRESS_START_COLOR = Color.parseColor("#e53e3e");
    private static final int PROGRESS_END_COLOR = Color.parseColor("#f97316");
    private static final int SPINNER_COLOR = Color.parseColor("#4CAF50");

    public LoadingOverlayView(Context context) {
        super(context);
        init();
    }

    public LoadingOverlayView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public LoadingOverlayView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init();
    }

    private void init() {
        mBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        mBgPaint.setColor(BG_COLOR);
        mBgPaint.setStyle(Paint.Style.FILL);

        mTitlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        mTitlePaint.setColor(TITLE_COLOR);
        mTitlePaint.setTextAlign(Paint.Align.CENTER);
        mTitlePaint.setTextSize(dp2px(24));

        mMessagePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        mMessagePaint.setColor(MESSAGE_COLOR);
        mMessagePaint.setTextAlign(Paint.Align.CENTER);
        mMessagePaint.setTextSize(dp2px(14));

        mProgressBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        mProgressBgPaint.setColor(Color.parseColor("#374151"));
        mProgressBgPaint.setStyle(Paint.Style.FILL);

        mProgressPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        mProgressPaint.setStyle(Paint.Style.FILL);

        mSpinnerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        mSpinnerPaint.setColor(SPINNER_COLOR);
        mSpinnerPaint.setStyle(Paint.Style.STROKE);
        mSpinnerPaint.setStrokeCap(Paint.Cap.ROUND);

        mStrokeWidth = dp2px(3);
        mProgressBarHeight = dp2px(4);
        mMaxProgressBarWidth = 1080f;

        startSpinnerAnimation();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        int width = getWidth();
        int height = getHeight();

        canvas.drawColor(Color.argb((int) (mAlpha * 204), 0, 0, 0));

        String title = "HvideoEngine";
        float titleY = height / 2f - dp2px(60);
        canvas.drawText(title, width / 2f, titleY, mTitlePaint);

        String subtitle = "Initializing System...";
        float subtitleY = titleY + dp2px(30);
        mMessagePaint.setTextSize(dp2px(14));
        canvas.drawText(subtitle, width / 2f, subtitleY, mMessagePaint);

        float progressWidth = Math.min(width * 0.70f, mMaxProgressBarWidth);
        float progressLeft = width / 2f - progressWidth / 2f;
        float progressTop = height / 2f + dp2px(20);
        float progressRight = width / 2f + progressWidth / 2f;
        float progressBottom = progressTop + mProgressBarHeight;

        RectF progressRect = new RectF(progressLeft, progressTop, progressRight, progressBottom);
        canvas.drawRoundRect(progressRect, mProgressBarHeight / 2, mProgressBarHeight / 2, mProgressBgPaint);

        if (mCurrentProgress > 0) {
            LinearGradient gradient = new LinearGradient(
                    progressLeft, 0,
                    progressLeft + (progressRight - progressLeft) * mCurrentProgress / 100f, 0,
                    PROGRESS_START_COLOR, PROGRESS_END_COLOR,
                    Shader.TileMode.CLAMP
            );
            mProgressPaint.setShader(gradient);

            float currentProgressRight = progressLeft + (progressRight - progressLeft) * mCurrentProgress / 100f;
            RectF currentProgressRect = new RectF(progressLeft, progressTop, currentProgressRight, progressBottom);
            canvas.drawRoundRect(currentProgressRect, mProgressBarHeight / 2, mProgressBarHeight / 2, mProgressPaint);
        } else {
            mProgressPaint.setShader(null);
        }

        String progressText = mCurrentProgress + "%";
        float progressTextY = progressBottom + dp2px(25);
        mMessagePaint.setTextSize(dp2px(16));
        canvas.drawText(progressText, width / 2f, progressTextY, mMessagePaint);

        float messageY = progressTextY + dp2px(25);
        mMessagePaint.setTextSize(dp2px(12));
        canvas.drawText(mCurrentMessage, width / 2f, messageY, mMessagePaint);

        float spinnerCenterX = width / 2f;
        float spinnerCenterY = messageY + dp2px(50);
        float spinnerRadius = dp2px(20);
        float indicatorRadius = dp2px(3.2f);
        double spinnerRadians = Math.toRadians(mSpinnerAngle - 90f);
        float indicatorX = spinnerCenterX + (float) (Math.cos(spinnerRadians) * spinnerRadius);
        float indicatorY = spinnerCenterY + (float) (Math.sin(spinnerRadians) * spinnerRadius);

        mSpinnerPaint.setShader(null);
        mSpinnerPaint.setStrokeWidth(mStrokeWidth);
        mSpinnerPaint.setAlpha(90);
        canvas.drawCircle(spinnerCenterX, spinnerCenterY, spinnerRadius, mSpinnerPaint);

        mSpinnerPaint.setStyle(Paint.Style.FILL);
        mSpinnerPaint.setColor(Color.WHITE);
        mSpinnerPaint.setAlpha(255);
        canvas.drawCircle(indicatorX, indicatorY, indicatorRadius, mSpinnerPaint);
        mSpinnerPaint.setColor(SPINNER_COLOR);
        mSpinnerPaint.setStyle(Paint.Style.STROKE);
    }

    public void updateProgress(int progress, String message) {
        if (progress < 0) progress = 0;
        if (progress > 100) progress = 100;

        if (mProgressAnimator != null) {
            mProgressAnimator.cancel();
        }

        mProgressAnimator = ValueAnimator.ofInt(mCurrentProgress, progress);
        mProgressAnimator.setDuration(300);
        mProgressAnimator.setInterpolator(new DecelerateInterpolator());
        mProgressAnimator.addUpdateListener(animation -> {
            mCurrentProgress = (int) animation.getAnimatedValue();
            postInvalidateOnAnimation();
        });
        mProgressAnimator.start();

        mCurrentMessage = message != null ? message : "";
        postInvalidateOnAnimation();
    }

    public void fadeOut() {
        ValueAnimator fadeAnimator = ValueAnimator.ofFloat(1.0f, 0.0f);
        fadeAnimator.setDuration(300);
        fadeAnimator.addUpdateListener(animation -> {
            mAlpha = (float) animation.getAnimatedValue();
            postInvalidateOnAnimation();
        });
        fadeAnimator.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                stopSpinnerAnimation();
                setVisibility(View.GONE);
            }
        });
        fadeAnimator.start();
    }

    public void reset() {
        mCurrentProgress = 0;
        mCurrentMessage = "正在启动...";
        mAlpha = 1.0f;
        mSpinnerAngle = 0f;
        setVisibility(View.VISIBLE);
        startSpinnerAnimation();
        postInvalidateOnAnimation();
    }

    private void startSpinnerAnimation() {
        if (mSpinnerRunning) {
            return;
        }
        mSpinnerRunning = true;
        mLastSpinnerFrameTimeMs = 0L;
        Choreographer.getInstance().postFrameCallback(mSpinnerFrameCallback);
    }

    private void stopSpinnerAnimation() {
        if (!mSpinnerRunning) {
            return;
        }
        mSpinnerRunning = false;
        mLastSpinnerFrameTimeMs = 0L;
        Choreographer.getInstance().removeFrameCallback(mSpinnerFrameCallback);
    }

    private int dp2px(float dpValue) {
        final float scale = getContext().getResources().getDisplayMetrics().density;
        return (int) (dpValue * scale + 0.5f);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        startSpinnerAnimation();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        if (mProgressAnimator != null) {
            mProgressAnimator.cancel();
        }
        stopSpinnerAnimation();
    }

    @Override
    protected void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        if (visibility == View.VISIBLE) {
            startSpinnerAnimation();
        } else {
            stopSpinnerAnimation();
        }
    }
}
