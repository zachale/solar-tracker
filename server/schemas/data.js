import mongoose, { Schema } from "mongoose";

const dataLogScheme = new Schema(
  {
    time: Date,
    temperature: Number,
    extended_percent: Number,
    status: String,
    wind_speed: Number,
    button_panel: {
      settings: Boolean,
      wind_sensor: Boolean,
      away: Boolean,
    },
  },
  { timestamps: true, collection: "datalogs" }
);

export const DataLog = mongoose.model("DataLog", dataLogScheme);
