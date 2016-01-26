//
// Created by LinKun on 9/13/15.
//

#include "SLAM/Alignment.h"
#include "SLAM/Calibrator.h"
#include "SLAM/Transformation.h"

#include "SLAM/ArucoMarkerUtils.h"

#include <limits>

#include <Core/Serialize.h>
#include <Core/Utility.h>
#include <Core/MyMath.h>

#include <boost/tuple/tuple.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#include <QMessageBox>
#include <QMap>
#include <QFileDialog>
#include <QThread>
#include <QTime>
#include <QWaitCondition>

namespace NiS {

	SlamComputer::SlamComputer ( std::shared_ptr < KeyFrames > keyframes_ptr , QObject * parent ) :
			running_flag_ ( true ) ,
			has_answer_ ( false ) ,
			is_computation_configured_ ( false ) ,
			is_data_initialized_ ( false ) {

		keyframes_ptr_ = keyframes_ptr;

	}

	void SlamComputer::SetDataDir ( const QDir & data_dir ) {

		data_dir_ = data_dir;
	}

	void SlamComputer::StartCompute ( ) {

		running_flag_ = true;

		if ( auto keyframes = keyframes_ptr_.lock ( ) ) {

			std::cout << "SlamComputer thread : " << QThread::currentThreadId ( ) << std::endl;
			std::cout << "SlamComputer thread - data size : " << keyframes->size ( ) << std::endl;

			if ( keyframes->size ( ) < 2 ) {
				return;
			}


			QTime timer;
			timer.start ( );

			emit Message ( "Computation begins..." );

			switch ( options_.type_ ) {
				case TrackingType::OneByOne:
					tracker_ptr_ = std::unique_ptr < OneByOneTracker > ( new OneByOneTracker );
					break;
				case TrackingType::FixedFrameCount:
					tracker_ptr_ = std::unique_ptr < FixedIntervalTracker > ( new FixedIntervalTracker );
					break;
				case TrackingType::PcaKeyFrame:
					tracker_ptr_ = std::unique_ptr < PcaKeyFrameTracker > ( new PcaKeyFrameTracker );
					break;
				case TrackingType::Unknown:
					emit Message ( "Setup computation options at first." );
					return;
			}

			tracker_ptr_->SetKeyframes ( keyframes );
			tracker_ptr_->SetOptions ( options_ );

			switch ( converter_choice_ ) {
				case 0:
					tracker_ptr_->SetConverter ( xtion_converter_ );
					break;
				case 1:
					tracker_ptr_->SetConverter ( aist_converter_ );
					break;
				default:
					return;
			}

			tracker_ptr_->Initialize ( );

			emit SetProgressRange ( 1 , keyframes->size ( ) - 1 );

			connect ( tracker_ptr_.get ( ) , SIGNAL( SetProgressValue ( int ) ) , this , SIGNAL ( SetProgressValue ( int ) ) );

			do {
				tracker_ptr_->ComputeNext ( );
			}
			while ( tracker_ptr_->Update ( ) );

			emit Message ( QString ( "Done computing %1 frames. (used %2)" )
					               .arg ( keyframes->size ( ) )
					               .arg ( ConvertTime ( timer.elapsed ( ) ) ) );

			WriteCache ( timer.elapsed ( ) );
			WriteResult ( );
		}
	}

	void SlamComputer::StartGenerateAnswer ( ) {

		if ( auto keyframes = keyframes_ptr_.lock ( ) ) {

			if ( keyframes->size ( ) < 2 ) {
				return;
			}

			aruco::MarkerDetector marker_detector;

			QTime timer;
			timer.start ( );

			all_markers_points_pairs_.clear ( );

			emit SetProgressRange ( 1 , keyframes->size ( ) - 1 );

			for ( auto i = 1 ; i < keyframes->size ( ) ; ++i ) {

				auto & keyframe1 = keyframes->at ( i - 1 );
				auto & keyframe2 = keyframes->at ( i );

				Markers markers1;
				Markers markers2;

				marker_detector.detect ( keyframe1.GetColorImage ( ) , markers1 );
				marker_detector.detect ( keyframe2.GetColorImage ( ) , markers2 );

				Points points1;
				Points points2;

				boost::tie ( points1 , points2 ) = ArucoMarkerUtils::CreatePoints ( markers1 , markers2 , keyframe1 , keyframe2 );

				all_markers_points_pairs_.push_back ( std::make_pair ( points1 , points2 ) );

				auto matrix = ComputeTransformationMatrix ( points2 , points1 , 100 ,
				                                            options_.options_one_by_one.threshold_outlier ,
				                                            options_.options_one_by_one.threshold_inlier );

				keyframe2.SetAnswerAlignmentMatrix ( std::move ( Convert_OpenCV_Matx44f_To_GLM_mat4 ( matrix ) ) );

				emit SetProgressValue ( i );
			}

			emit Message ( QString ( "Done generating answers of  %1 frames. (used %2)" )
					               .arg ( keyframes_.size ( ) )
					               .arg ( ConvertTime ( timer.elapsed ( ) ) ) );
			has_answer_ = true;
		}
	}

	bool SlamComputer::WriteResult ( const std::pair < glm::vec3 , glm::vec3 > & point_pair ) {

		time_t time_stamp = time ( nullptr );

		QString result_name_prefix;

		std::cout << options_.type_ << std::endl;

		switch ( options_.type_ ) {
			case TrackingType::OneByOne:
				result_name_prefix = QString ( "%1_%2" ).arg ( time_stamp ).arg ( "OneByOne" );
				break;
			case TrackingType::PcaKeyFrame:
				result_name_prefix = QString ( "%1_%2" ).arg ( time_stamp ).arg ( "PcaKeyFrame" );
				break;
			case TrackingType::FixedFrameCount:
				result_name_prefix = QString ( "%1_%2" ).arg ( time_stamp ).arg ( "FixedFrameCount" );
				break;
			case TrackingType::Unknown:
				emit Message ( "No result to be written." );
				return false;

		}

		QString data_folder_path = data_dir_.absolutePath ( );
		QDir    dir ( data_folder_path + "/Result" );
		if ( !dir.exists ( ) ) dir.mkdir ( data_folder_path + "/Result" );

		std::string   result_file_name = QString (
				data_folder_path + "/Result/" + result_name_prefix + ".txt" ).toStdString ( );
		std::ofstream out ( result_file_name );

		if ( out ) {

			out << "Current tracker type : " << static_cast<int>(options_.type_) << std::endl;

			switch ( options_.type_ ) {

				case TrackingType::OneByOne: {
					out << options_.options_one_by_one.Output ( ).toStdString ( ) << std::endl;
					break;
				}
				case TrackingType::PcaKeyFrame: {
					out << options_.options_pca_keyframe.Output ( ).toStdString ( ) << std::endl;
					break;
				}
				case TrackingType::FixedFrameCount : {
					out << options_.options_fixed_frame_count.Output ( ).toStdString ( ) << std::endl;
					break;
				}
				default:
					break;
			}

			out << "point1 " << glm::vec4 ( point_pair.first , 1.0f );
			out << "point2 " << glm::vec4 ( point_pair.second , 1.0f );

			out.close ( );

			return true;
		}

		return false;
	}

	bool SlamComputer::WriteResult ( ) {

		if ( auto keyframes = keyframes_ptr_.lock ( ) ) {
			time_t time_stamp = time ( nullptr );

			QString result_name_prefix;

			std::cout << options_.type_ << std::endl;

			switch ( options_.type_ ) {
				case TrackingType::OneByOne:
					result_name_prefix = QString ( "%1_%2" ).arg ( time_stamp ).arg ( "OneByOne" );
					break;
				case TrackingType::PcaKeyFrame:
					result_name_prefix = QString ( "%1_%2" ).arg ( time_stamp ).arg ( "PcaKeyFrame" );
					break;
				case TrackingType::FixedFrameCount:
					result_name_prefix = QString ( "%1_%2" ).arg ( time_stamp ).arg ( "FixedFrameCount" );
					break;
				case TrackingType::Unknown:
					emit Message ( "No result to be written." );
					return false;
			}

			QString data_folder_path = data_dir_.absolutePath ( );
			QDir    dir ( data_folder_path + "/Result" );
			if ( !dir.exists ( ) ) dir.mkdir ( data_folder_path + "/Result" );

			std::string   result_file_name = QString (
					data_folder_path + "/Result/" + result_name_prefix + ".txt" ).toStdString ( );
			std::ofstream out ( result_file_name );

			if ( out ) {

				out << "Current tracker type : " << static_cast<int>(options_.type_) << std::endl;

				switch ( options_.type_ ) {

					case TrackingType::OneByOne: {
						out << options_.options_one_by_one.Output ( ).toStdString ( ) << std::endl;
						break;
					}
					case TrackingType::PcaKeyFrame: {
						out << options_.options_pca_keyframe.Output ( ).toStdString ( ) << std::endl;
						break;
					}
					case TrackingType::FixedFrameCount : {
						out << options_.options_fixed_frame_count.Output ( ).toStdString ( ) << std::endl;
						break;
					}
					default:
						break;
				}

				out << "CAUTION: The look at point has been translated to the origin." << std::endl;
				out << "Position X (estimation),Position Y (estimation),Position Z (estimation),"
						"Position X (marker),Position Y (marker),Position Z (marker),"
						"Translation Error,"
						"LookatPoint X (estimation),LookatPoint Y (estimation),LookatPoint Z (estimation),"
						"LookatPoint X (marker),LookatPoint Y (marker),LookatPoint Z (marker),"
						"Rotation Error" << std::endl;

				auto position_estimation     = glm::vec3 ( );
				auto position_marker         = glm::vec3 ( );
				auto lookat_point_estimation = glm::vec3 ( 0.0f , 0.0f , 1.0f );
				auto lookat_point_marker     = glm::vec3 ( 0.0f , 0.0f , 1.0f );

				auto accumulated_matrix_estimation = glm::mat4 ( );
				auto accumulated_matrix_marker     = glm::mat4 ( );

//				for ( const auto & keyframe : * keyframes ) {
//
//					accumulated_matrix_estimation *= keyframe.GetAlignmentMatrix ( );
//					accumulated_matrix_marker *= keyframe.GetAnswerAlignmentMatrix ( );
//
//					const auto _position_estimation = accumulated_matrix_estimation * glm::vec4 ( position_estimation , 1.0f );
//					const auto _position_marker     = accumulated_matrix_marker * glm::vec4 ( position_marker , 1.0f );
//
//					const auto _lookat_point_estimation =
//							           accumulated_matrix_estimation * glm::vec4 ( lookat_point_estimation , 1.0f );
//					const auto _lookat_point_marker     = accumulated_matrix_marker * glm::vec4 ( lookat_point_marker , 1.0f );
//
//					const auto __lookat_point_estimation = _lookat_point_estimation - _position_estimation;
//					const auto __lookat_point_marker     = _lookat_point_marker - _position_marker;
//
//					out << _position_estimation.x << "," << _position_estimation.y << "," << _position_estimation.z << ",";
//					out << _position_marker.x << "," << _position_marker.y << "," << _position_marker.z << ",";
//
//					if ( keyframe.IsUsed ( ) ) out << glm::length ( _position_estimation - _position_marker ) << ",";
//					else out << 0 << ",";
//
//					out << __lookat_point_estimation.x << "," << __lookat_point_estimation.y << "," <<
//					__lookat_point_estimation.z << ",";
//					out << __lookat_point_marker.x << "," << __lookat_point_marker.y << "," << __lookat_point_marker.z <<
//					",";
//
//					if ( keyframe.IsUsed ( ) ) out << glm::angle ( __lookat_point_estimation , __lookat_point_marker ) << std::endl;
//					else out << 0 << std::endl;
//
//				}


				// Last -> first
				for ( const auto & keyframe : * keyframes ) {
					accumulated_matrix_estimation *= keyframe.GetAlignmentMatrix ( );
				}

				auto last_frame_1 = ( keyframes->end ( ) - 1 )->Clone ( );
				auto last_frame_2 = ( keyframes->end ( ) - 1 )->Clone ( );

				// Apply estimated matrix to last frame
				auto point_image2 = last_frame_2.GetPointImage ( );

				for ( auto row = 0 ; row < point_image2.rows ; ++row ) {
					for ( auto col = 0 ; col < point_image2.cols ; ++col ) {

						auto point = point_image2.at < cv::Vec3f > ( row , col );

						auto transformed_point = accumulated_matrix_estimation * glm::vec4 ( point ( 0 ) , point ( 1 ) , point ( 2 ) , 1.0f );

						point_image2.at < cv::Vec3f > ( row , col ) = cv::Vec3f ( transformed_point.x , transformed_point.y , transformed_point.z );
					}
				}
				last_frame_2.SetPointImage ( point_image2 );

				// Apply real matrix to last frame

				auto answer_matrix_of_last_frame = ComputeTransformationMatrixOf ( last_frame_1 , ( * keyframes )[ 0 ] );

				auto point_image1 = last_frame_1.GetPointImage ( );

				for ( auto row = 0 ; row < point_image1.rows ; ++row ) {
					for ( auto col = 0 ; col < point_image1.cols ; ++col ) {

						auto point = point_image1.at < cv::Vec3f > ( row , col );

						auto transformed_point = answer_matrix_of_last_frame * glm::vec4 ( point ( 0 ) , point ( 1 ) , point ( 2 ) , 1.0f );

						point_image1.at < cv::Vec3f > ( row , col ) = cv::Vec3f ( transformed_point.x , transformed_point.y , transformed_point.z );
					}
				}
				last_frame_1.SetPointImage ( point_image1 );

				// Compute the error

				auto error_matrix = ComputeTransformationMatrixOf ( last_frame_2 , last_frame_1 );

				auto m = Convert_GLM_mat4_To_OpenCV_Matx44f ( error_matrix );

				cv::Matx33f r = cv::Mat ( m ) ( cv::Rect ( 0 , 0 , 3 , 3 ) );
				cv::Vec3f   t = cv::Mat ( m , false ) ( cv::Rect ( 0 , 3 , 3 , 1 ) );

				cout << "m : " << m << endl;
				cout << r << endl;
				cout << t << endl;

				auto q = CreateQuaternion ( r );

				auto radian = acos ( q ( 0 ) ) * 2;
				auto degree = radian * 180.0f / M_PI;
				auto x      = q ( 1 ) / sin ( 0.5 * radian );
				auto y      = q ( 2 ) / sin ( 0.5 * radian );
				auto z      = q ( 3 ) / sin ( 0.5 * radian );

				cout << "Rotation by Quaternion (t; x y z) :" << q ( 0 ) << " " << q ( 1 ) << " " << q ( 2 ) << " " << q ( 3 ) << endl;
				cout << "Translation Vector : " << t ( 0 ) << " " << t ( 1 ) << " " << t ( 2 ) << endl;
				cout << "Distance           : " << glm::length ( glm::vec3 ( t ( 0 ) , t ( 1 ) , t ( 2 ) ) ) << endl;
				cout << "Rotation Axis      : " << x << ", " << y << ", " << z << endl;
				cout << "Rotation Degree    : " << degree << endl;

				out << "Rotation by Quaternion (t; x y z) :" << q ( 0 ) << " " << q ( 1 ) << " " << q ( 2 ) << " " << q ( 3 ) << endl;
				out << "Translation Vector : " << t ( 0 ) << " " << t ( 1 ) << " " << t ( 2 ) << endl;
				out << "Distance           : " << glm::length ( glm::vec3 ( t ( 0 ) , t ( 1 ) , t ( 2 ) ) ) << endl;
				out << "Rotation Axis      : " << x << ", " << y << ", " << z << endl;
				out << "Rotation Degree    : " << degree << endl;

				out.close ( );
				return true;
			}

		}
		return false;
	}

	bool SlamComputer::CheckPreviousResult ( ) {

		std::cout << "Checking privious result." << std::endl;

		result_cache_path_ = data_dir_.absolutePath ( ) + "/Cache";

		QDir dir ( result_cache_path_ );
		if ( !dir.exists ( ) ) {
			dir.mkdir ( result_cache_path_ );
			return false;
		}

		QStringList filter_list;
		filter_list.push_back ( QString ( "*.cache" ) );

		return !dir.entryInfoList ( filter_list ).empty ( );
	}

	void SlamComputer::UsePreviousResult ( const QString & result_cache_name ) {

		if ( auto keyframes = keyframes_ptr_.lock ( ) ) {

			if ( keyframes->empty ( ) ) {
				emit Message ( "No data found, cannot apply matrices." );
			}

			QTime timer;
			timer.start ( );

			ComputationResultCache cache;

			bool load_succeeded = LoadComputationResultCache ( result_cache_name.toStdString ( ) , cache );
			if ( !load_succeeded ) { return; }

			auto data_set_name    = cache.data_set_name;
			auto computation_time = cache.computation_time;

			options_ = cache.options;

			for ( auto i = 0 ; i < cache.indices.size ( ) ; ++i ) {

				auto id                = cache.indices[ i ];
				auto is_used           = cache.used_status[ i ];
				auto estimation_matrix = cache.estimation_matrices[ i ];
				auto marker_matrix     = cache.marker_matrices[ i ];

				auto & kf = keyframes->at ( id );

				kf.SetId ( id );
				kf.SetUsed ( is_used );
				kf.SetAlignmentMatrix ( estimation_matrix );
				kf.SetAnswerAlignmentMatrix ( marker_matrix );

			}

			emit Message ( QString ( "Done loading %1 frames' results. (used %2)" )
					               .arg ( keyframes->size ( ) )
					               .arg ( ConvertTime ( timer.elapsed ( ) ) ) );
		}
	}

	void SlamComputer::StopCompute ( ) {

		running_flag_ = false;
	}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	bool SaveMatricesInfo ( const std::string & file_name , const MatricesInfo & info ) {

		std::ofstream out ( file_name , std::ios::binary );

		if ( out ) {
			namespace bio = ::boost::iostreams;

			bio::filtering_ostream f;
			f.push ( bio::gzip_compressor ( ) );
			f.push ( out );

			boost::archive::binary_oarchive ar ( out );
			ar << info;

			return true;
		}

		return false;
	}

	bool LoadMatricesInfo ( const std::string & file_name , MatricesInfo & info ) {

		std::ifstream in ( file_name , std::ios::binary );

		if ( in ) {
			namespace bio = ::boost::iostreams;

			bio::filtering_istream f;
			f.push ( bio::gzip_decompressor ( ) );
			f.push ( in );

			boost::archive::binary_iarchive ar ( in );
			ar >> info;

			return true;
		}

		return false;
	}


	glm::mat4 SlamComputer::ComputeTransformationMatrixOf ( const KeyFrame & from , const KeyFrame & to ) {

		auto keyframes = std::shared_ptr < KeyFrames > ( new KeyFrames ( 2 ) );

		( * keyframes )[ 0 ] = to.Clone ( );
		( * keyframes )[ 1 ] = from.Clone ( );

		OneByOneTracker tracker;
		tracker.SetKeyframes ( keyframes );
		tracker.SetOptions ( options_ );

		tracker.Initialize ( );

		switch ( converter_choice_ ) {
			case 0:
				tracker.SetConverter ( xtion_converter_ );
				break;
			case 1:
				tracker.SetConverter ( aist_converter_ );
				break;
			default:
				return glm::mat4 ( );
		}


		do {
			tracker.ComputeNext ( );
		}
		while ( tracker.Update ( ) );

		return ( * keyframes )[ 1 ].GetAlignmentMatrix ( );
	}

}

